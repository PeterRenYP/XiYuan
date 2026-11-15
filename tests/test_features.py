"""使用离线真实微型模型验证新增功能的数值、事务与版本一致性。"""
import copy
import io
import json
from pathlib import Path

import pytest
import torch

from observatory.features import activation_summary
from observatory.model import QwenAdapter, inspect_model
from observatory.protocol import Emitter, ProtocolError, command
from observatory.traces import TraceStore
from observatory.worker import Runtime
from observatory.control import Cancelled
from test_runtime import create_local_model


@pytest.fixture
def runtime(tmp_path):
    """每次测试使用独立目录，所有线程在失败时也正常退出。"""
    path = tmp_path / "模型"
    create_local_model(path, layers=2)
    current = Runtime(Emitter("features", io.StringIO()))
    current.adapter = QwenAdapter.load(path, "cpu", "float32")
    yield current
    current.handle(command("shutdown"))
    current.thread.join(10)


def call(runtime, action, **payload):
    """模拟界面携带最新权重版本的真实功能请求。"""
    return runtime.features.dispatch(dict(action=action, expected_weights_revision=runtime.adapter.weights_revision, **payload))


def test_vector_search_similarity_and_versions(runtime):
    """文本与编号指向同一条目，重叠补丁启停和重做保持基座语义。"""
    assert call(runtime, "search", query="你好")["rows"][0]["id"] == 2
    assert call(runtime, "search", query="2", exact=True)["rows"][0]["text"] == "你好"
    vector = call(runtime, "vector", token_id=2)
    assert vector["dimension"] == 16 and len(vector["values"]) == 16
    similar = call(runtime, "similar", token_id=2)
    assert similar["rows"][0]["id"] == 2
    assert similar["rows"][0]["score"] == pytest.approx(1, abs=1e-6)
    original = runtime.adapter.model.get_input_embeddings().weight[2, :3].detach().clone()
    edit = dict(token_id=2, start=0, stop=3, values=[.25], kind="set")
    preview = call(runtime, "vector.preview", **edit)
    assert preview["after"] == [.25] * 3
    assert torch.equal(runtime.adapter.model.get_input_embeddings().weight[2, :3], original)
    first = call(runtime, "vector.apply", **edit)
    second = call(runtime, "vector.apply", token_id=2, start=1, stop=2, values=[2], kind="scale")
    assert call(runtime, "vector", token_id=2)["values"][:3] == [.25, .5, .25]
    call(runtime, "versions.action", operation="toggle", patch_id=first["patches"][0]["id"])
    row = runtime.adapter.model.get_input_embeddings().weight[2, :3]
    torch.testing.assert_close(row, original * torch.tensor([1., 2., 1.]))
    call(runtime, "versions.action", operation="undo")
    assert call(runtime, "vector", token_id=2)["values"][:3] == [.25, .5, .25]
    call(runtime, "versions.action", operation="redo")
    loaded = QwenAdapter.load(runtime.adapter.info["path"], "cpu", "float32")
    torch.testing.assert_close(loaded.model.get_input_embeddings().weight[2, :3], row)
    assert loaded.info["model_revision"] == runtime.adapter.info["model_revision"]
    from observatory.versions import Versions
    assert Versions(loaded).summary()["compatible"]


def test_shared_weight_edited_once(tmp_path):
    """共享输入输出采用磁盘唯一目标，缩放不会重复执行。"""
    path = tmp_path / "共享模型"
    model, tokenizer = create_local_model(path)
    model.config.tie_word_embeddings = True
    model.tie_weights()
    model.save_pretrained(path)
    current = Runtime(Emitter("shared", io.StringIO()))
    try:
        current.adapter = QwenAdapter.load(path, "cpu", "float32")
        weight = current.adapter.model.get_input_embeddings().weight
        before = float(weight[2, 0].detach())
        assert call(current, "vector", token_id=2)["shared"]
        call(current, "vector.apply", token_id=2, start=0, stop=1, kind="scale", values=[2])
        assert float(weight[2, 0].detach()) == pytest.approx(before*2)
        assert float(current.adapter.model.get_output_embeddings().weight[2, 0].detach()) == pytest.approx(before*2)
        call(current, "versions.action", operation="undo")
        assert float(weight[2, 0].detach()) == before
    finally:
        current.handle(command("shutdown"))
        current.thread.join(10)


def test_vocabulary_restart_undo_and_export(runtime, tmp_path):
    """普通词表重命名在磁盘、重启、撤销与导出副本保持一致。"""
    call(runtime, "vocabulary.apply", token_id=2, text="您好")
    assert runtime.adapter.tokenizer.encode("您好", add_special_tokens=False) == [2]
    loaded = QwenAdapter.load(runtime.adapter.info["path"], "cpu", "float32")
    assert loaded.tokenizer.encode("您好", add_special_tokens=False) == [2]
    from observatory.versions import Versions
    assert Versions(loaded).summary()["compatible"]
    call(runtime, "versions.action", operation="undo")
    assert runtime.adapter.tokenizer.encode("你好", add_special_tokens=False) == [2]
    call(runtime, "versions.action", operation="redo")
    project = tmp_path / "项目"
    project.mkdir()
    report = call(runtime, "export.verify", project_root=str(project), destination=str(tmp_path/"副本"),
        samples=[dict(input="您好", target="用户", criterion="probability", threshold=0, max_new_tokens=2)])
    assert report["export"]["passed"]
    assert (project / "features/samples.json").is_file()


def test_import_idempotent_and_invalid_values(runtime, tmp_path):
    """同一补丁重复导入不累计；无效数值和过期版本不能写入。"""
    call(runtime, "vector.apply", token_id=2, start=0, stop=1, kind="scale", values=[2])
    file = tmp_path / "补丁.json"
    call(runtime, "versions.export", path=str(file))
    before = call(runtime, "vector", token_id=2)["values"]
    assert call(runtime, "versions.import", path=str(file))["unchanged"]
    assert call(runtime, "vector", token_id=2)["values"] == before
    for payload in (dict(start=-1,stop=1,values=[1]),dict(start=0,stop=2,values=[1,2,3]),dict(start=0,stop=1,values=[float("nan")])):
        with pytest.raises(ProtocolError):
            call(runtime,"vector.apply",token_id=2,**payload)
    with pytest.raises(ProtocolError, match="版本"):
        runtime.features.dispatch(dict(action="vector.apply",token_id=2,start=0,stop=1,values=[1],expected_weights_revision="w-old"))
    with pytest.raises(ProtocolError):
        call(runtime,"vocabulary.apply",token_id=1,text="结束")
    assert call(runtime, "vector", token_id=2)["values"] == before


def test_activation_summary_and_scope(runtime, tmp_path):
    """项目标准采集在未选中层仍有排名，本轮输入排除历史。"""
    known = torch.tensor([[[1.,-5.,0.],[3.,2.,1.]]])
    summary = activation_summary(known,[4,5])
    assert summary[1].tolist()==[3.,-5.,1.]
    assert summary[4].tolist()==[5.,4.,5.]
    project=tmp_path/"项目"
    project.mkdir()
    (project/"project.json").write_text(json.dumps({"storage":{"total_bytes":64*1024**2,"per_run_bytes":32*1024**2}}),encoding="utf-8")
    runtime.run(command("run.start",{"messages":[{"role":"user","content":"用户 用户"},{"role":"assistant","content":"模型"},{"role":"user","content":"你好"}],
        "mode":"observe","generation":{"temperature":0,"max_new_tokens":2},"project_root":str(project),
        "trace_root":str(project/"records"),"capture":{"policy":"standard","layer":0,"disk_budget_bytes":32*1024**2}}))
    one=call(runtime,"activation",scope="input",metric="peak")
    two=call(runtime,"activation",scope="input",metric="peak")
    assert one==two and one["layers"]==2 and one["effective_tokens"]==1
    assert all(r["samples"]==1 for r in one["rows"])
    best=one["rows"][0]
    if best["layer"]==0:
        context,refs=runtime.store.get_operation("base",best["forward_id"],best["layer"],"mlp")
        value=runtime.store.load(refs["baseline"])[0,context["positions"].index(best["position"]),best["neuron"]]
        assert float(value)==best["peak"]
    before=runtime.store.root
    runtime.store.close()
    runtime.store=TraceStore(before,readonly=True)
    assert call(runtime,"activation",scope="input",metric="peak")==one


def test_activation_selected_reply_isolated_and_exact(runtime, tmp_path):
    """历史回复的精确选区独立读取，不能混入当前会话、其他 Token 或其他记录。"""
    project=tmp_path/"来源项目"
    def record(name, values, policy="standard"):
        """用确定数值构造两条来源不同的真实存储记录。"""
        store=TraceStore(project/"sessions"/name/"records"/name)
        store.save_manifest(run_id=name, capture_policy=policy, token_ids=[2,3], user_positions=[0,1])
        store.operation({"branch_id":"base","forward_id":"f1","layer_index":0,"operation_id":"mlp","positions":[2,3,4]},
            {"baseline":torch.tensor([values]),"output":torch.tensor([values])})
        store.flush()
        return store
    current=record("当前会话",[[100.,200.],[300.,400.],[500.,600.]])
    selected=record("历史会话",[[1.,2.],[3.,4.],[5.,6.]])
    selected.close();runtime.store=current
    payload=dict(trace_path=str(selected.root),run_id="历史会话",project_root=str(project),positions=[3],scope="positions",metric="peak")
    result=call(runtime,"activation",**payload)
    assert runtime.store is current and not current.closed
    assert result["effective_tokens"]==1 and result["layers"]==1
    assert {row["position"] for row in result["rows"]}=={3}
    assert sorted(row["peak"] for row in result["rows"])==[3.,4.]
    saved=json.loads(Path(result["path"]).read_text(encoding="utf-8"))
    assert saved["positions"]==[3] and saved["trace_path"]==str(selected.root)
    detail=call(runtime,"activation.detail",layer=0,neuron=1,**payload)
    assert detail["rows"][0]["before"]==4. and len(detail["rows"])==1
    with pytest.raises(ProtocolError,match="不匹配"):
        call(runtime,"activation",**(payload|{"run_id":"其他回复"}))
    with pytest.raises(ProtocolError,match="不属于"):
        call(runtime,"activation",**(payload|{"trace_path":str(tmp_path)}))
    normal=record("普通会话",[[1.,2.],[3.,4.],[5.,6.]],"none");normal.close()
    with pytest.raises(ProtocolError,match="普通对话模式"):
        call(runtime,"activation",**(payload|{"trace_path":str(normal.root),"run_id":"普通会话"}))


def test_regression_restores_state(runtime, tmp_path):
    """原版对照期间临时恢复参数，异常或正常结束都还原编辑版。"""
    call(runtime,"vector.apply",token_id=2,start=0,stop=3,values=[.5])
    expected=runtime.adapter.model.get_input_embeddings().weight.detach().clone()
    runtime.adapter.edit_rules=[dict(layer_index=0,neuron_index=0,kind="zero",value=0,scope="all")]
    rules=copy.deepcopy(runtime.adapter.edit_rules)
    project=tmp_path/"项目"
    project.mkdir()
    report=call(runtime,"regression",project_root=str(project),samples=[dict(input="你好",max_new_tokens=2)])
    assert report["rows"][0]["edited"]["passed"] is None
    torch.testing.assert_close(expected,runtime.adapter.model.get_input_embeddings().weight)
    assert runtime.adapter.edit_rules==rules
    failed=call(runtime,"regression",project_root=str(project),samples=[dict(input="你好",criterion="contains",expected="",max_new_tokens=1)])
    assert failed["rows"][0]["edited"]["status"]=="failed"
    torch.testing.assert_close(expected,runtime.adapter.model.get_input_embeddings().weight)
    assert runtime.adapter.edit_rules==rules


def test_version_and_tokenizer_transaction_rolls_back(runtime, monkeypatch):
    """分词器与版本文件提交失败时全部回滚，不残留新文件或内存状态。"""
    import observatory.editing as editing
    root=runtime.features.versions().path.parent
    before=(root/"tokenizer.json").read_bytes()
    original=editing.os.replace
    def fail(source, destination):
        """在词表替换后注入一次版本索引提交故障。"""
        if str(destination).endswith("observatory-versions.json"):
            raise OSError("测试提交失败")
        return original(source,destination)
    monkeypatch.setattr(editing.os,"replace",fail)
    with pytest.raises(OSError):
        call(runtime,"vocabulary.apply",token_id=2,text="您好")
    assert (root/"tokenizer.json").read_bytes()==before
    assert not (root/"observatory-versions.json").exists()
    assert runtime.adapter.tokenizer.encode("你好",add_special_tokens=False)==[2]


def test_cancelled_regression_restores_weights(runtime, tmp_path, monkeypatch):
    """停止原版评测后，实际参数、分词器和临时规则仍回到编辑版。"""
    call(runtime,"vector.apply",token_id=2,start=0,stop=1,values=[.75])
    expected=runtime.adapter.model.get_input_embeddings().weight.detach().clone()
    tokenizer=runtime.adapter.tokenizer
    project=tmp_path/"取消项目"
    project.mkdir()
    def cancel(adapter,sample):
        """在基座参数已切入时模拟用户取消。"""
        raise Cancelled()
    monkeypatch.setattr(runtime.features,"evaluate",cancel)
    with pytest.raises(Cancelled):
        call(runtime,"regression",project_root=str(project),samples=[dict(input="你好")])
    torch.testing.assert_close(expected,runtime.adapter.model.get_input_embeddings().weight)
    assert runtime.adapter.tokenizer is tokenizer


def test_regular_sample_and_stale_preview(runtime, tmp_path):
    """常规样例可按概率降幅判断；旧内容预览不能绕过版本检查。"""
    project=tmp_path/"回归项目"
    project.mkdir()
    result=call(runtime,"regression",project_root=str(project),samples=[dict(type="regular",input="你好",target="用户",max_probability_drop=0,max_new_tokens=1)])
    assert result["rows"][0]["edited"]["passed"] is True
    preview=call(runtime,"vector.preview",token_id=2,start=0,stop=1,values=[.5])
    call(runtime,"vector.apply",token_id=2,start=0,stop=1,values=[.25])
    with pytest.raises(ProtocolError,match="预览来源"):
        call(runtime,"vector.apply",token_id=2,start=0,stop=1,values=[.5],expected_model_revision=preview["expected_model_revision"])


def test_same_draft_does_not_scale_twice(runtime):
    """预览生成的稳定编号让重复保存同一缩放草稿保持幂等。"""
    before=call(runtime,"vector",token_id=2)["values"][0]
    preview=call(runtime,"vector.preview",token_id=2,start=0,stop=1,values=[2],kind="scale")
    payload=dict(token_id=2,start=0,stop=1,values=[2],kind="scale",patch_id=preview["patch_id"])
    call(runtime,"vector.apply",**payload)
    second=call(runtime,"vector.apply",**payload)
    assert second["unchanged"]
    assert call(runtime,"vector",token_id=2)["values"][0]==pytest.approx(before*2)


def test_legacy_parameter_edit_cannot_replay_old_base(runtime):
    """旧单系数入口只改内存时，也不能绕过新版本基座校验。"""
    call(runtime,"versions")
    runtime.adapter.patch(dict(layer_index=0,neuron_index=1,dimension=0,kind="gate",value=.5,
        expected_weights_revision=runtime.adapter.weights_revision))
    assert not call(runtime,"versions")["compatible"]
    with pytest.raises(ProtocolError,match="未纳入版本"):
        call(runtime,"vector.apply",token_id=2,start=0,stop=1,values=[.75])


def test_activation_full_snapshot_paging_filters_and_export(runtime, tmp_path, monkeypatch):
    """超过旧百条上限后仍可翻到末页；搜索和排序查询快照，完整导出不受分页影响。"""
    store=TraceStore(tmp_path/"trace",budget=1)
    runtime.store=store
    store.save_manifest(run_id="full-result",capture_policy="full",token_ids=[2,3],user_positions=[0,1])
    for layer in range(2):
        values=torch.arange(1,258,dtype=torch.float32).reshape(1,257).repeat(2,1)*(layer+1)
        store.operation({"branch_id":"base","forward_id":"f0","layer_index":layer,"operation_id":"mlp","positions":[0,1]},
            {"baseline":values,"output":values})
    first=call(runtime,"activation",scope="all",metric="peak",paged=True,page_size=37,project_root=str(tmp_path),client_revision=7)
    assert first["total"]==514 and first["page_count"]==14 and len(first["rows"])==37
    assert first["rows"][0]["score"]==514 and first["client_revision"]==7
    saved=json.loads(Path(first["path"]).read_text(encoding="utf-8"))
    assert len(saved["rows"])==514

    def forbidden(*args,**kwargs):
        """查询已经完成的分析时，禁止重新读取张量和计算排名。"""
        raise AssertionError("分页查询不应重新计算激活")
    monkeypatch.setattr(runtime.features,"rank_store",forbidden)
    query=dict(analysis_id=first["analysis_id"],project_root=str(tmp_path),page_size=37)
    last=call(runtime,"activation.query",**query,page=100000)
    assert last["page"]==14 and len(last["rows"])==33
    assert last["rows"][-1]["score"]==1
    subset=call(runtime,"activation.query",**query,layer_min=1,layer_max=1,score_min=500,samples_min=2,sort="neuron",ascending=True)
    assert subset["total"]==8 and [r["neuron"] for r in subset["rows"]]==list(range(249,257))
    assert call(runtime,"activation.query",**query,samples_min=3)["total"]==0
    matches=call(runtime,"activation.query",**query,query="256")
    assert {(r["layer"],r["neuron"]) for r in matches["rows"]}=={(0,256),(1,256)}
    text=call(runtime,"activation.query",**query,query="解码未保存")
    assert text["total"]==514
    with pytest.raises(ProtocolError,match="不属于当前项目"):
        call(runtime,"activation.query",analysis_id=first["analysis_id"],project_root=str(tmp_path/"other"))
    with pytest.raises(ProtocolError,match="无效"):
        call(runtime,"activation.query",**query,sort="score; DROP TABLE ranked")
    with pytest.raises(ProtocolError,match="无效"):
        call(runtime,"activation.query",**query,score_min=float("nan"))
    reopened=call(runtime,"activation.open",path=first["path"],project_root=str(tmp_path),page_size=500)
    assert reopened["total"]==514 and len(reopened["rows"])==500
    assert reopened["analysis_id"]!=first["analysis_id"]
    # 打开失败不能使旧结果快照失效。
    with pytest.raises(ProtocolError):
        call(runtime,"activation.open",path=str(tmp_path/"missing.json"),project_root=str(tmp_path))
    assert call(runtime,"activation.query",**query)["total"]==514


def test_activation_detail_pages_beyond_old_4096_limit(runtime,tmp_path):
    """数值详情可查询旧截断点后的全部位置，选区与分页都保持来源身份。"""
    store=TraceStore(tmp_path/"detail-trace")
    runtime.store=store
    store.save_manifest(run_id="detail-source",capture_policy="full")
    values=torch.arange(5003,dtype=torch.float32).reshape(-1,1).repeat(1,2)
    store.operation({"branch_id":"base","forward_id":"f0","layer_index":0,"operation_id":"mlp","positions":list(range(5003))},
        {"baseline":values,"output":values+1})
    result=call(runtime,"activation.detail",layer=0,neuron=1,page=6,page_size=1000,run_id="detail-source",client_revision=11)
    assert result["total"]==5003 and result["page_count"]==6 and len(result["rows"])==3
    assert result["rows"][-1]["position"]==5002 and result["rows"][-1]["after"]==5003
    assert result["run_id"]=="detail-source" and result["client_revision"]==11
    selected=call(runtime,"activation.detail",layer=0,neuron=1,positions=[4999,5000,5001,5002],page_size=2,page=2)
    assert selected["total"]==4 and [r["position"] for r in selected["rows"]]==[5001,5002]


def test_activation_progress_identity_counts_and_completion(runtime, tmp_path):
    """真实运行队列持续发送关联进度，阶段计数完整且最终才返回成功。"""
    store = TraceStore(tmp_path / "progress-trace")
    runtime.store = store
    store.save_manifest(run_id="progress", capture_policy="full")
    values = torch.arange(257, dtype=torch.float32).reshape(1, -1)
    store.operation({"branch_id": "base", "forward_id": "f0", "layer_index": 0,
        "operation_id": "mlp", "positions": [0]}, {"baseline": values, "output": values})
    request = command("feature.request", {"action": "activation", "scope": "all", "paged": True,
        "project_root": str(tmp_path), "page_size": 37})
    runtime.handle(request)
    runtime.tasks.join()
    events = [json.loads(line) for line in runtime.emitter.stream.getvalue().splitlines()]
    progress = [event for event in events if event["name"] == "feature.progress"]
    assert progress and all(event["request_id"] == request["request_id"] for event in progress)
    for stage, total in (("正在读取张量并计算统计", 1), ("正在汇总神经元排名", 257),
                         ("正在保存分析结果", 257), ("正在建立结果索引", 257)):
        counts = [event["payload"]["completed"] for event in progress if event["payload"]["message"] == stage]
        assert counts == sorted(counts) and counts[0] == 0 and counts[-1] == total
    assert any(event["payload"]["total"] is None for event in progress)
    assert len(progress) < 60
    assert events[-1]["name"] == "feature.result" and events[-1]["payload"]["total"] == 257
    assert runtime.features.progress_request is None and runtime.features.progress_action is None


def test_activation_cancel_keeps_old_snapshot_and_cleans_new_files(runtime, tmp_path):
    """写入新快照期间取消，旧结果可查询，新报告与临时文件不发布。"""
    store = TraceStore(tmp_path / "cancel-trace")
    runtime.store = store
    store.save_manifest(run_id="cancel", capture_policy="full")
    values = torch.ones(1, 257)
    store.operation({"branch_id": "base", "forward_id": "f0", "layer_index": 0,
        "operation_id": "mlp", "positions": [0]}, {"baseline": values, "output": values})
    old = call(runtime, "activation", scope="all", paged=True, project_root=str(tmp_path))
    files = set((tmp_path / "features").iterdir())
    cache_files = set(Path(runtime.features.analysis_cache.name).iterdir())
    listener = runtime.emitter.listener

    def cancel_on_index(event):
        """从控制通道取消真实任务，验证停止响应而非直接抛出测试异常。"""
        listener(event)
        if event["name"] == "feature.progress" and event["payload"]["message"] == "正在建立结果索引":
            runtime.handle(command("run.cancel"))

    runtime.emitter.listener = cancel_on_index
    request = command("feature.request", {"action": "activation", "scope": "all", "paged": True,
        "project_root": str(tmp_path)})
    runtime.handle(request)
    runtime.tasks.join()
    runtime.emitter.listener = listener
    events = [json.loads(line) for line in runtime.emitter.stream.getvalue().splitlines()
              if json.loads(line)["request_id"] == request["request_id"]]
    assert events[-1]["kind"] == "error" and events[-1]["payload"]["code"] == "CANCELLED"
    assert "取消" in events[-1]["payload"]["message"]
    assert set((tmp_path / "features").iterdir()) == files
    assert set(Path(runtime.features.analysis_cache.name).iterdir()) == cache_files
    runtime.features.cancelled = False
    assert call(runtime, "activation.query", analysis_id=old["analysis_id"], project_root=str(tmp_path))["total"] == 257


def test_activation_cancel_during_hash_and_report_save(runtime, tmp_path):
    """大张量校验和逐条保存均设取消检查点，未完成文件不能覆盖有效报告。"""
    store = TraceStore(tmp_path / "hash-trace")
    runtime.store = store
    ref = store.tensor(torch.ones(1024, 1024))
    store.flush()
    checks = 0

    def stop_hash():
        """越过加载前检查，确认在实际文件哈希循环中也能停止。"""
        nonlocal checks
        checks += 1
        if checks == 5:
            raise Cancelled()

    with pytest.raises(Cancelled):
        store.load(ref, cancel=stop_hash)
    assert checks == 5
    path = tmp_path / "old.json"
    path.write_text('{"old":true}', encoding="utf-8")
    runtime.features.cancelled = True
    with pytest.raises(Cancelled):
        runtime.features.save_activation(path, {"rows": [{"score": 1}]})
    assert json.loads(path.read_text(encoding="utf-8")) == {"old": True}
    assert not list(tmp_path.glob("*.tmp"))
    runtime.features.cancelled = False
