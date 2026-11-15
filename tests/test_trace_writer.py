"""验证慢盘隔离、快照一致性和后台写入异常，不以推理结果代替持久化验证。"""
import threading

import pytest
import torch

from observatory.protocol import ProtocolError
from observatory.traces import TraceStore


def test_slow_writer_does_not_block_capture_and_snapshot_is_immutable(tmp_path, monkeypatch):
    """磁盘任务被阻塞时采集仍返回，后续修改原张量不改变已接纳的副本。"""
    import safetensors.torch
    original = safetensors.torch.save
    entered, release = threading.Event(), threading.Event()

    def slow_save(values):
        """用屏障模拟慢盘，避免依赖机器速度设置脆弱的耗时阈值。"""
        entered.set()
        assert release.wait(5)
        return original(values)

    monkeypatch.setattr(safetensors.torch, "save", slow_save)
    store = TraceStore(tmp_path)
    value = torch.ones(2, 3)
    try:
        ref = store.tensor(value)
        assert ref and entered.wait(2)
        # 写线程尚未完成第一个文件，主线程仍能接纳第二个快照。
        second = store.tensor(value * 2)
        assert second
        value.zero_()
        release.set()
        torch.testing.assert_close(store.load(ref), torch.ones(2, 3))
        torch.testing.assert_close(store.load(second), torch.full((2, 3), 2.))
    finally:
        release.set()
        store.close()


def test_queue_is_bounded_and_index_commits_are_batched(tmp_path):
    """超过队列预算的单块独占落盘，不丢采集；普通事件批量提交。"""
    store = TraceStore(tmp_path)
    try:
        store.queue_limit = 16
        ref=store.tensor(torch.zeros(100))
        assert ref and not store.incomplete
        assert store.load(ref).numel()==100
        assert store.pending_bytes==0
        store.queue_limit = 64 * 1024**2
        for seq in range(100):
            store.event({"seq": seq, "name": "operation.completed", "payload": {}})
        store.flush()
        assert store.db.execute("SELECT COUNT(*) FROM events").fetchone()[0] == 100
        assert store.commit_count < 25
    finally:
        store.close()


def test_writer_failure_is_reported_and_missing_snapshot_is_not_published(tmp_path, monkeypatch):
    """真实写入失败在同步屏障处上报，数据库不出现不存在文件的有效引用。"""
    import safetensors.torch

    def broken_save(values):
        """模拟磁盘写入错误。"""
        raise OSError("模拟磁盘写入失败")

    monkeypatch.setattr(safetensors.torch, "save", broken_save)
    store = TraceStore(tmp_path)
    try:
        ref = store.tensor(torch.ones(3))
        with pytest.raises(ProtocolError, match="模拟磁盘写入失败"):
            store.flush()
        assert store.incomplete and store.load(ref) is None
    finally:
        store.close()


def test_many_captures_exceed_legacy_quota_and_queue_without_loss(tmp_path):
    """很小的旧采集额度和队列预算均不能造成后续张量或操作记录丢失。"""
    store=TraceStore(tmp_path,budget=1)
    store.queue_limit=1024
    try:
        for index in range(96):
            refs=store.operation({"branch_id":"base","forward_id":f"f{index}","layer_index":0,"operation_id":"mlp","positions":[index]},
                {"baseline":torch.full((1,64),float(index))})
            assert refs["baseline"]
        store.flush()
        assert not store.incomplete
        assert store.db.execute("SELECT count(*) FROM tensors").fetchone()[0]==96
        assert store.db.execute("SELECT count(*) FROM operations").fetchone()[0]==96
        for index in (0,48,95):
            _,refs=store.get_operation("base",f"f{index}",0,"mlp")
            torch.testing.assert_close(store.load(refs["baseline"]),torch.full((1,64),float(index)))
    finally:
        store.close()


def test_single_tensor_larger_than_legacy_limit_roundtrips(tmp_path):
    """真实写入超过旧单块 128 MiB 限制的张量，分块校验后完整读回。"""
    store=TraceStore(tmp_path,budget=16*1024*1024)
    try:
        value=torch.zeros(128*1024*1024+1,dtype=torch.uint8)
        value[-1]=123
        ref=store.tensor(value)
        assert ref and not store.incomplete
        loaded=store.load(ref)
        assert loaded.numel()==value.numel() and loaded[-1].item()==123
        assert store.bytes_written>128*1024*1024
    finally:
        store.close()
