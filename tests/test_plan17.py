"""开发计划的事务与数值验收；使用独立微型模型，避免修改用户模型。"""
import copy

import pytest
import torch

from test_features import runtime, call
from observatory.protocol import ProtocolError


def rule(target="model", **changes):
    """生成完整规则，测试停用、重复和混合提交时不省略字段。"""
    return dict(target=target, enabled=True, layer_index=0, neuron_index=1,
        kind="scale", value=2., scope="all", token_position=0, projection="down", dimension=0) | changes


def test_rules_idempotent_disable_and_independent_patch(runtime):
    """重复提交不累计；清空规则保留同一区域的独立补丁。"""
    parameter = runtime.adapter.parameter(0, "down")
    original = parameter[:, 1].detach().clone()
    from observatory.versions import Versions
    versions = runtime.features.versions()
    independent = versions.tensor_patch(dict(name=versions.canonical(parameter), axis=1, index=1, start=0, stop=1), [3.], "scale")
    versions.add([independent])
    draft = [rule()]
    preview = call(runtime, "rules.preview", rules=draft)
    assert preview["targets"] and preview["model_count"] == 1
    first = call(runtime, "rules.apply", rules=draft)
    after = parameter.detach().clone()
    second = call(runtime, "rules.apply", rules=draft)
    assert torch.equal(parameter, after)
    assert len(first["patches"]) == len(second["patches"])
    call(runtime, "rules.apply", rules=[rule(enabled=False)])
    expected = original.clone()
    expected[0] *= 3
    torch.testing.assert_close(parameter[:, 1], expected)
    call(runtime, "rules.apply", rules=[])
    torch.testing.assert_close(parameter[:, 1], expected)
    call(runtime, "rules.apply", rules=draft)
    torch.testing.assert_close(parameter[:, 1], expected * 2)


def test_rules_disabled_duplicate_and_atomic_failure(runtime):
    """停用项仍拦截重复；混合提交失败时原权重与临时配置都不改变。"""
    with pytest.raises(ProtocolError, match="重复"):
        call(runtime, "rules.preview", rules=[rule(enabled=False), rule(value=4)])
    before = runtime.adapter.parameter(0, "down").detach().clone()
    call(runtime, "rules.apply", rules=[rule("temporary", value=.5)])
    old = copy.deepcopy(runtime.adapter.edit_rules)
    with pytest.raises(ProtocolError):
        call(runtime, "rules.apply", rules=[rule(value=1e100), rule("temporary", value=.1)])
    assert runtime.adapter.edit_rules == old
    assert torch.equal(runtime.adapter.parameter(0, "down"), before)


def test_rule_change_then_undo_restores_previous_version(runtime):
    """修改规则后撤销能恢复旧规则的权重，停用不是数值逆运算。"""
    before = runtime.adapter.parameter(0, "down")[:, 1].detach().clone()
    call(runtime, "rules.apply", rules=[rule(value=2)])
    call(runtime, "rules.apply", rules=[rule(value=3)])
    torch.testing.assert_close(runtime.adapter.parameter(0, "down")[:, 1], before * 3)
    call(runtime, "versions.action", operation="undo")
    torch.testing.assert_close(runtime.adapter.parameter(0, "down")[:, 1], before * 2)


def test_mixed_rule_undo_restores_temporary_configuration(runtime):
    """临时和权重属于同一版本，撤销与重做同时恢复两种配置。"""
    first=[rule(value=2),rule("temporary",value=.25)]
    second=[rule(value=3),rule("temporary",value=.5)]
    call(runtime,"rules.apply",rules=first)
    call(runtime,"rules.apply",rules=second)
    previous=call(runtime,"versions.action",operation="undo")
    assert previous["rules"]==first and runtime.adapter.edit_rules[0]["value"]==.25
    call(runtime,"versions.action",operation="redo")
    assert runtime.adapter.edit_rules[0]["value"]==.5


def test_imported_rules_have_independent_owner(runtime,tmp_path):
    """另一模型副本导入的规则补丁，不受本机空规则列表影响。"""
    import shutil
    from observatory.model import QwenAdapter
    clone=tmp_path/"clone";shutil.copytree(runtime.adapter.info["path"],clone)
    call(runtime,"rules.apply",rules=[rule()])
    expected=runtime.adapter.parameter(0,"down").detach().clone()
    path=tmp_path/"rules.json";call(runtime,"versions.export",path=str(path))
    runtime.adapter=QwenAdapter.load(clone,"cpu","float32")
    imported=call(runtime,"versions.apply",path=str(path))
    assert imported["patches"][0]["owner"]=="import"
    call(runtime,"rules.apply",rules=[])
    assert torch.equal(runtime.adapter.parameter(0,"down"),expected)
    assert call(runtime,"versions.apply",path=str(path))["unchanged"]


def test_recovery_failure_blocks_run_and_export(runtime,tmp_path,monkeypatch):
    """磁盘回滚再次失败时，模型明确锁定，不能继续推理或导出。"""
    import observatory.editing as editing
    from observatory.protocol import command
    recover=editing.recover_save;replace=editing.os.replace;calls=0
    def fail_recovery(root):
        """只让正式写入后的恢复失败，启动前校验正常。"""
        nonlocal calls
        calls+=1
        if calls>1:raise OSError("恢复磁盘故障")
        return recover(root)
    def fail_write(source,target):
        """注入正式分块发布故障，迫使执行恢复路径。"""
        if "staged" in str(source):raise OSError("提交磁盘故障")
        return replace(source,target)
    monkeypatch.setattr(editing,"recover_save",fail_recovery)
    monkeypatch.setattr(editing.os,"replace",fail_write)
    with pytest.raises(ProtocolError,match="恢复失败"):
        call(runtime,"rules.apply",rules=[rule()])
    assert runtime.adapter.recovery_required
    with pytest.raises(ProtocolError,match="恢复失败"):
        runtime.run(command("run.start",{}))
    with pytest.raises(ProtocolError,match="不能导出"):
        runtime.export_model(command("model.export",{"destination":str(tmp_path/"export")}))


def test_large_result_chunk_roundtrip():
    """跨通信帧的报告完整重组，中文和浮点不在分块边界损坏。"""
    import base64,io,json
    from observatory.protocol import Emitter,MAX_LINE
    stream=io.StringIO();expected={"rows":[{"text":"原始激活","value":i/65537} for i in range(45000)]}
    Emitter("large",stream).send("feature.result",expected,{"request_id":"large"},"result")
    messages=[json.loads(line) for line in stream.getvalue().splitlines()]
    assert len(messages)>1 and all(m["name"]=="result.chunk" for m in messages)
    assert all(len(json.dumps(m,ensure_ascii=False).encode())<MAX_LINE for m in messages)
    result=json.loads(b"".join(base64.b64decode(m["payload"]["data"]) for m in messages))
    assert result["payload"]==expected and result["request_id"]=="large"


def test_version_toggle_syncs_rule_configuration(runtime):
    """版本页停用后，规则页和撤销重做保持同一真实配置。"""
    before=runtime.adapter.parameter(0,"down")[:,1].detach().clone()
    saved=call(runtime,"rules.apply",rules=[rule()])
    disabled=call(runtime,"versions.action",operation="toggle",patch_id=saved["patches"][0]["id"])
    assert not disabled["rules"][0]["enabled"]
    torch.testing.assert_close(runtime.adapter.parameter(0,"down")[:,1],before)
    restored=call(runtime,"versions.action",operation="undo")
    assert restored["rules"][0]["enabled"]
    torch.testing.assert_close(runtime.adapter.parameter(0,"down")[:,1],before*2)


def test_debug_boundary_matching_continue_and_cancel(runtime):
    """未命中不暂停，计算前与条件后的边界可分别继续和取消。"""
    import threading
    from observatory.control import StepController,Cancelled
    from observatory.debugging import validate_points
    hit=threading.Event();events=[];outcomes=[]
    def notify(name,payload):
        """记录实际边界并通知测试线程，避免依赖固定等待时间。"""
        events.append(payload);hit.set()
    controller=StepController("observe",notify)
    controller.configure(validate_points(runtime.adapter,[{"layer_index":1,"operation_id":"mlp","generation_step":0,"boundary":"before"}]))
    controller.checkpoint({"layer_index":0,"operation_id":"mlp","generation_step":0})
    assert not hit.is_set()
    thread=threading.Thread(target=lambda:controller.checkpoint({"layer_index":1,"operation_id":"mlp","generation_step":0}));thread.start()
    try:
        assert hit.wait(2) and events[-1]["boundary"]=="before"
        controller.set_mode("observe");thread.join(2);assert not thread.is_alive()
        hit.clear();controller.configure(validate_points(runtime.adapter,[{"layer_index":0,"operation_id":"mlp","boundary":"after","watch":"channel","index":1,"comparison":"gt","threshold":1}]))
        controller.after({"layer_index":0,"operation_id":"mlp"},torch.tensor([[[0.,.5]]]))
        assert not hit.is_set()
        def watched():
            """取消从真实条件暂停中退出，不吞掉协作取消状态。"""
            try:controller.after({"layer_index":0,"operation_id":"mlp"},torch.tensor([[[0.,-2.]]]))
            except Cancelled:outcomes.append("cancelled")
        thread=threading.Thread(target=watched);thread.start();assert hit.wait(2)
        assert events[-1]["boundary"]=="after" and events[-1]["observed_value"]==2
        controller.cancel();thread.join(2);assert outcomes==["cancelled"]
        with pytest.raises(ProtocolError):validate_points(runtime.adapter,[{"layer_index":0,"operation_id":"norm","boundary":"after","watch":"channel","index":1,"comparison":"gt","threshold":0}])
    finally:
        controller.cancel();thread.join(2)


def test_activation_other_conversation_baseline(runtime,tmp_path):
    """不同长度对话记录有效数量，以相同均值和频率方法比较同一候选。"""
    import json
    from observatory.protocol import command
    project=tmp_path/"baseline-project";project.mkdir();(project/"project.json").write_text(json.dumps({"storage":{"total_mib":64,"run_mib":16}}))
    def collect(text):
        """每次执行真实前向并保存独立记录，用于重新打开的基线比较。"""
        runtime.run(command("run.start",{"messages":[{"role":"user","content":text}],"mode":"observe","generation":{"max_new_tokens":2,"temperature":0},"project_root":str(project),"trace_root":str(project/"records"),"capture":{"policy":"standard","layer":0,"disk_budget_bytes":16*1024**2}}))
        return str(runtime.store.root)
    baseline=collect("你好 你好 你好");collect("你好")
    result=call(runtime,"activation",scope="input",metric="mean",baseline=baseline,project_root=str(project))
    assert result["baseline_tokens"]==3 and result["effective_tokens"]==1
    assert all(r["baseline_samples"]==3 and r["samples"]==1 for r in result["rows"])
    assert all(r["difference"]==pytest.approx(r["score"]-r["baseline_score"]) for r in result["rows"])
    reopened=call(runtime,"report.read",path=result["path"],project_root=str(project))
    assert reopened["baseline_provenance"]["messages"][0]["content"]=="你好 你好 你好"
