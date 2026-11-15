"""验证安装目录内置 Python 的真实推理，以及关联启动的独立文件模式。"""
from __future__ import annotations
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import numpy as np
import psutil
from safetensors.numpy import save_file
from mcp import ClientSession,StdioServerParameters
from mcp.client.stdio import stdio_client


async def verify(installation,model,output):
    """子进程清空 Python 路径并在独立目录启动，源码仅由测试进程读取。"""
    output.mkdir(parents=True,exist_ok=False);checks={};windows=os.environ.get('SystemRoot',r'C:\Windows')
    env=os.environ.copy()
    for key in ('PYTHONPATH','PYTHONHOME','VIRTUAL_ENV','MODEL_OBSERVATORY_ROOT'):env.pop(key,None)
    env.update(PATH=windows+os.pathsep+windows+r'\System32',QT_QPA_PLATFORM='offscreen',QT_QPA_PLATFORM_PLUGIN_PATH=str(installation/'platforms'),
        MODEL_OBSERVATORY_MCP_DIR=str(output/'bridge'),MODEL_OBSERVATORY_SETTINGS_DIR=str(output/'settings'),PYTHONUTF8='1')
    fixture=output/'关联权重.safetensors';save_file({'a':np.arange(4,dtype=np.float32)},str(fixture));original=fixture.read_bytes()
    for phase in ('文件','模型'):
        log=(output/(phase+'.log')).open('w',encoding='utf-8')
        process=subprocess.Popen([str(installation/'Xiyuan.exe')]+(['--open-file',str(fixture)] if phase=='文件' else []),cwd=output,env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            for _ in range(150):
                if list((output/'bridge').glob('*.json')):break
                if process.poll() is not None:raise RuntimeError('安装版启动失败')
                await asyncio.sleep(.1)
            async with stdio_client(StdioServerParameters(command=str(installation/'Xiyuan-MCP.exe'),cwd=str(output),env=env)) as (reader,writer):
                async with ClientSession(reader,writer) as session:
                    await session.initialize()
                    async def call(tool_name,**arguments):
                        """只接受标准工具的成功结果。"""
                        result=await session.call_tool('observatory_'+tool_name,arguments);data=result.structuredContent or json.loads(result.content[0].text)
                        if result.isError:raise AssertionError(data)
                        return data['data']
                    async def wait(submission,event_name):
                        """等待真实完成事件，绝不把 accepted 当完成。"""
                        cursor=submission['event_cursor'];deadline=time.monotonic()+180
                        while time.monotonic()<deadline:
                            result=await call('get_events',after_cursor=cursor,limit=100);cursor=result['next_cursor']
                            for event in result['items']:
                                if event['name']=='request.failed':raise AssertionError(event)
                                if event['name']==event_name:return event
                            await asyncio.sleep(.1)
                        raise TimeoutError(event_name)
                    instances=await call('list_instances');await call('select_instance',instance_id=instances['instances'][0]['instance_id'])
                    if phase=='文件':
                        for _ in range(100):
                            state=await call('get_state')
                            if state.get('file_workspace',{}).get('path') and not state['file_workspace'].get('busy',True):break
                            await asyncio.sleep(.1)
                        assert not state['conversation_visible'] and not state['loaded']
                        submission=await call('list_weight_tensors',query='a');await wait(submission,'file.result')
                        preview=await call('preview_weight_edit',name='a',start=1,values=[.25]);await wait(preview,'file.result')
                        result=await call('get_feature_result',request_id=preview['request_id'])
                        saved=await call('apply_weight_edit',name='a',start=1,values=[.25],expected_revision=result['revision']);await wait(saved,'file.result')
                        saved=await call('get_feature_result',request_id=saved['request_id'])
                        await wait(await call('restore_weight_file',backup=saved['backup'],expected_revision=saved['revision']),'file.result')
                        assert fixture.read_bytes()==original
                        rejected=await session.call_tool('observatory_configure_debug',{'points':[]});assert rejected.isError
                        checks['文件关联入口可修改备份恢复且拒绝项目调试']=True
                        # 独立文件进程存在，但主工作台没有启动推理或监控。
                        children=psutil.Process(process.pid).children(recursive=True)
                        commands=[p.cmdline() for p in children]
                        assert any('--file-worker' in c for c in commands) and not any('--worker' in c or '--monitor' in c for c in commands)
                        checks['独立文件模式不启动推理进程']=True
                    else:
                        await wait(await call('create_project',directory=str(output/'项目'),name='安装版真实推理',model_path=str(model),device='cuda:0',max_tokens=3),'model.ready')
                        await wait(await call('new_session'),'session.activated')
                        await wait(await call('send_message',text='请说你好。',max_tokens=3,temperature=0),'run.finished')
                        history=await call('get_history');assert any(i.get('role')=='assistant' and i.get('content') for i in history['items'])
                        checks['安装版独立加载真实Qwen并生成']=True
                        children=psutil.Process(process.pid).children(recursive=True);modules=[]
                        for child in children:
                            if '--worker' in child.cmdline():modules.extend(m.path for m in child.memory_maps() if 'python312.dll' in m.path.lower())
                        assert modules and all(Path(p).is_relative_to(installation) for p in modules),modules
                        checks['推理实际使用安装目录内的Python动态库']=True
                        checks['外部Python与开发环境路径已移除']=True
        finally:
            process.terminate()
            try:process.wait(8)
            except subprocess.TimeoutExpired:process.kill();process.wait()
            log.close()
    (output/'安装版运行环境验收.json').write_text(json.dumps({'pass':all(checks.values()),'checks':checks},ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(checks,ensure_ascii=False))


if __name__=='__main__':asyncio.run(verify(*(Path(v).resolve() for v in sys.argv[1:4])))
