"""经标准 MCP 管道验证计划新增功能，全部模型和项目写入隔离目录。"""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
from safetensors.numpy import save_file
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from test_runtime import create_local_model


async def verify(executable, output):
    """每个提交等待对应请求的最终事件，再查询分页结果。"""
    root=Path(__file__).resolve().parents[1]
    output.mkdir(parents=True,exist_ok=False)
    model=output/"model";create_local_model(model,layers=2)
    file=output/"file.safetensors"
    save_file({"weights":np.random.default_rng(42).normal(size=65536).astype(np.float32)},str(file))
    original=file.read_bytes()
    env=os.environ.copy();env.update(MODEL_OBSERVATORY_ROOT=str(root),MODEL_OBSERVATORY_MCP_DIR=str(output/"bridge"),
        MODEL_OBSERVATORY_SETTINGS_DIR=str(output/"settings"),QT_QPA_PLATFORM="offscreen",
        QT_QPA_PLATFORM_PLUGIN_PATH=str(root/".cache/qt/6.8.3/mingw_64/plugins/platforms"),PYTHONUTF8="1",PYTHONIOENCODING="utf-8")
    env["PATH"]=str(root/".cache/qt/6.8.3/mingw_64/bin")+os.pathsep+str(root/".cache/qt/Tools/mingw1310_64/bin")+os.pathsep+env["PATH"]
    if os.environ.get("XIYUAN_TEST_PACKAGED") == "1":
        # 已安装程序只看到系统目录，禁止借用开发虚拟环境或源码模块。
        for key in ("PYTHONPATH","PYTHONHOME","VIRTUAL_ENV","MODEL_OBSERVATORY_ROOT"):
            env.pop(key,None)
        windows=os.environ.get("SystemRoot",r"C:\Windows")
        env["PATH"]=os.path.join(windows,"System32")+os.pathsep+windows
        env["QT_QPA_PLATFORM_PLUGIN_PATH"]=str(executable.parent/"platforms")
    checks={};called=set()
    log=(output/"client.log").open("w",encoding="utf-8")
    process=subprocess.Popen([str(executable)],cwd=root,env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        for _ in range(150):
            if list((output/"bridge").glob("*.json")):break
            if process.poll() is not None:raise RuntimeError("客户端提前退出")
            await asyncio.sleep(.1)
        mcp_executable = os.environ.get("XIYUAN_TEST_MCP_EXE")
        async with stdio_client(StdioServerParameters(command=mcp_executable or str(root/".venv-mcp/Scripts/python.exe"),args=[] if mcp_executable else [str(root/"run_mcp.py")],cwd=str(root),env=env)) as (reader,writer):
            async with ClientSession(reader,writer) as session:
                await session.initialize()

                async def call(tool_name,**arguments):
                    """调用真实已注册工具，并拒绝把执行错误当成功。"""
                    called.add(tool_name);response=await session.call_tool("observatory_"+tool_name,arguments)
                    data=response.structuredContent or json.loads(response.content[0].text)
                    assert not response.isError,data
                    return data["data"]

                async def wait(submission,name):
                    """在事件分页中匹配请求，其他任务事件不提前结束本次等待。"""
                    cursor=submission["event_cursor"];deadline=time.monotonic()+60
                    while time.monotonic()<deadline:
                        events=await call("get_events",after_cursor=cursor,limit=100)
                        assert not events["dropped"],events
                        cursor=events["next_cursor"]
                        for event in events["items"]:
                            matches=not submission.get("request_id") or event.get("request_id")==submission["request_id"]
                            if event["name"]=="request.failed" and matches:raise AssertionError(event)
                            if event["name"]==name and (matches or name in ("model.ready","session.activated","run.finished","step.waiting")):return event
                        await asyncio.sleep(.05)
                    raise TimeoutError(name)

                async def feature(tool_name,**arguments):
                    """查询最终缓存，同时验证异步请求身份。"""
                    submission=await call(tool_name,**arguments)
                    await wait(submission,"feature.result")
                    result=await call("get_feature_result",request_id=submission["request_id"])
                    if isinstance(result.get("rows"),dict) and "total" in result["rows"]:
                        result["rows"]=(await call("get_feature_result",request_id=submission["request_id"],field="rows",offset=0,limit=100))["items"]
                    return result

                async def file_action(tool_name,**arguments):
                    """文件操作共享客户端控制桥，具有独立文件版本。"""
                    submission=await call(tool_name,**arguments)
                    await wait(submission,"file.result")
                    return await call("get_feature_result",request_id=submission["request_id"])

                async def revisions():
                    """每次写入前从当前模型重新读取版本。"""
                    state=await call("get_state")
                    return {"expected_weights_revision":state["weights_revision"],"expected_model_revision":state["model_revision"]}

                instances=await call("list_instances");assert len(instances["instances"])==1
                await call("select_instance",instance_id=instances["instances"][0]["instance_id"])
                submitted=await call("create_project",directory=str(output/"project"),name="计划验收",model_path=str(model),device="cpu",max_tokens=3)
                await wait(submitted,"model.ready")
                await wait(await call("new_session"),"session.activated")
                assert (await call("get_state"))["mode"]=="normal"
                # 验证真正切到神经元页，并复用原生定位、暂停与清除选中。
                neuron_view = await call("show_neurons", layer_index=1, neuron_index=3, live=False)
                assert neuron_view["neuron_visible"] and neuron_view["neuron_selection"]["has_selection"]
                assert neuron_view["neuron_selection"]["neuron_index"] == 3 and not neuron_view["neuron_selection"]["live"]
                await call("select_view", layer_index=0, operation="mlp", neuron_index=5)
                neuron_view = await call("get_state")
                assert neuron_view["neuron_visible"] and neuron_view["neuron_selection"]["neuron_index"] == 5
                neuron_view = await call("show_neurons", **{"global": True, "live": True})
                assert neuron_view["neuron_visible"] and not neuron_view["neuron_selection"]["has_selection"]
                assert neuron_view["neuron_selection"]["live"]
                checks["MCP神经元页面定位与播放同步"] = True
                checks["新会话默认对话模式"]=True
                await call("set_mode",mode="observe")
                sent = await call("send_message", text="你好", max_tokens=2, temperature=0)
                await wait(sent, "run.finished")
                assert (await call("get_state"))["neuron_active_beams"] > 0
                token=await feature("search_tokens",query="你好",exact=True);assert token["rows"][0]["id"]==2
                vector=await feature("query_vector",token_id=2,space="input",start=0,count=16)
                output_vector=await feature("query_vector",token_id=2,space="output",start=0,count=16)
                assert vector["dimension"]==output_vector["dimension"]==16 and not vector["shared"]
                similar=await feature("search_similar_vectors",token_id=2,space="input",limit=3);assert similar["rows"][0]["id"]==2
                edit={"token_id":2,"space":"input","start":0,"stop":2,"values":[.25],"kind":"set"}
                preview=await feature("preview_vector",**edit);assert preview["after"]==[.25,.25]
                await feature("save_feature_drafts",items=[edit|{"action":"vector.apply","patch_id":preview["patch_id"]}|await revisions()])
                assert len((await feature("get_feature_drafts"))["items"])==1
                applied=await feature("apply_vector",**edit,patch_id=preview["patch_id"],**await revisions())
                await feature("change_version",operation="undo",**await revisions())
                await feature("change_version",operation="redo",**await revisions())
                assert (await feature("query_vector",token_id=2))["values"][:2]==[.25,.25]
                checks["词表搜索与输入输出向量查询修改撤销重做"]=True
                word=await feature("preview_vocabulary",token_id=2,text="您好")
                await feature("apply_vocabulary",token_id=2,text="您好",patch_id=word["patch_id"],**await revisions())
                assert (await feature("search_tokens",query="您好",exact=True))["rows"][0]["id"]==2
                await feature("change_version",operation="undo",**await revisions())
                patches=output/"patches.json"
                await feature("export_patches",path=str(patches));await feature("preview_patches",path=str(patches))
                imported=await feature("import_patches",path=str(patches),**await revisions());assert imported["unchanged"]
                checks["词表编辑与补丁导入导出幂等"]=True
                rule={"target":"temporary","enabled":True,"layer_index":0,"neuron_index":1,"kind":"zero","value":0,"scope":"all","token_position":0,"projection":"down","dimension":0}
                await feature("save_rule_draft",rules=[rule])
                await feature("preview_rules",rules=[rule])
                await feature("apply_rule_draft",rules=[rule],**await revisions())
                assert len((await call("get_rules"))["applied"])==1
                comparison=await feature("compare_branches",mode="fixed",steps=2,target="用户",layer=0,neuron=1,operation="mlp")
                assert comparison["rows"][0]["status"]=="available"
                assert all(p["after"]==0 for p in comparison["rows"][0]["channel"])
                await feature("compare_branches",mode="free",steps=2,target="用户",layer=0,neuron=1,operation="mlp")
                await feature("apply_rule_draft",rules=[],**await revisions())
                checks["规则完整替换与固定前缀真实通道对照"]=True
                # 新运行采集本版本，再对比同一份记录的一致排名。
                await wait(await call("send_message",text="你好",max_tokens=2,temperature=0),"run.finished")
                ranking=await feature("analyze_activation",scope="input",metric="normalized",layer=-1)
                candidate=ranking["rows"][0]
                detail=await feature("activation_detail",layer=candidate["layer"],neuron=candidate["neuron"])
                assert detail["rows"] and ranking["provenance"]["rules_snapshot"] is not None
                await call("select_view",layer_index=candidate["layer"],operation="mlp",neuron_index=candidate["neuron"],token_position=candidate["position"])
                checks["激活候选定位与原始曲线来源"]=True
                configured=await call("configure_debug",points=[{"layer_index":0,"operation_id":"mlp","boundary":"after","watch":"channel","index":1,"comparison":"ge","threshold":0}])
                assert configured["points"]
                submission=await call("send_message",text="你好",max_tokens=2,temperature=0)
                paused=await wait(submission,"step.waiting");assert paused["payload"]["boundary"]=="after"
                await call("continue_run");await wait(submission,"run.finished")
                await call("configure_debug",points=[])
                checks["通道条件监视暂停与继续"]=True
                samples=[{"input":"你好","target":"用户","criterion":"probability","threshold":0,"max_new_tokens":2,"generation":{"temperature":0,"seed":123}},
                    {"input":"你好","criterion":"exact","expected":"不可能的完整回答","max_new_tokens":2}]
                await feature("save_samples",items=samples);assert len((await feature("get_samples"))["items"])==2
                regression=await feature("run_regression",samples=samples)
                assert regression["rows"][0]["edited"]["passed"] and not regression["rows"][1]["edited"]["passed"]
                exported=await feature("verify_export",samples=samples[:1],destination=str(output/"export"));assert exported["export"]["passed"]
                reports=await feature("list_reports");assert len(reports["rows"])>=4
                reopened=await feature("open_report",path=regression["path"]);assert reopened["rows"]==regression["rows"]
                checks["项目样例与失败报告重开及独立副本验收"]=True
                files=await file_action("open_weight_file",path=str(file));assert files["total"]==1
                await file_action("list_weight_tensors",query="weight",offset=0,limit=10)
                submission=await call("read_weight_slice",name="weights",start=0,count=65536)
                await wait(submission,"file.result")
                page=await call("get_feature_result",request_id=submission["request_id"],field="values",offset=65530,limit=6);assert page["total"]==65536 and len(page["items"])==6
                pre=await file_action("preview_weight_edit",name="weights",start=2,values=[.25])
                saved=await file_action("apply_weight_edit",name="weights",start=2,values=[.25],expected_revision=pre["revision"])
                await file_action("compare_weight_files",other=str(model/"model.safetensors"))
                await file_action("restore_weight_file",backup=saved["backup"],expected_revision=saved["revision"])
                assert file.read_bytes()==original
                checks["独立文件编辑恢复与大结果分帧分页"]=True
                # 同一模型已被项目加载时，文件入口必须明确拒绝写入，不能返回空请求。
                opened=await file_action("open_weight_file",path=str(model/"model.safetensors"))
                if isinstance(opened["rows"],dict):
                    listed=await file_action("list_weight_tensors",query="embed_tokens",offset=0,limit=1)
                    tensor=listed["rows"][0]["name"]
                else:
                    tensor=opened["rows"][0]["name"]
                rejected=await session.call_tool("observatory_apply_weight_edit",{"name":tensor,"start":0,"values":[.25],"expected_revision":opened["revision"]})
                assert rejected.isError,(rejected.structuredContent or rejected.content)
                sessions=await call("list_sessions");current=sessions["current"]
                await call("rename_session",session_id=current,title="验收后的会话")
                assert any(s["title"]=="验收后的会话" for s in (await call("list_sessions"))["items"])
                await call("save_project")
                checks["会话重命名与项目保存"]=True
    finally:
        process.terminate()
        try:process.wait(8)
        except subprocess.TimeoutExpired:process.kill();process.wait()
        log.close()
        (output/"MCP新增功能验收.json").write_text(json.dumps({"pass":len(checks)==10,"checks":checks,"tools":sorted(called)},ensure_ascii=False,indent=2),encoding="utf-8")
    assert len(checks)==10,checks
    print(json.dumps(checks,ensure_ascii=False))


if __name__=="__main__":
    asyncio.run(verify(Path(sys.argv[1]).resolve(),Path(sys.argv[2]).resolve()))
