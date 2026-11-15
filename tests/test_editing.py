"""验证批量局部编辑的真实数值、磁盘事务及备份恢复。"""
import copy

import pytest
import torch

from observatory.editing import (apply_activations, digest, prepare_weights,
    restore_backup, save_current_model, validate_rules)
from observatory.model import QwenAdapter, inspect_model
from observatory.protocol import ProtocolError
from test_runtime import create_local_model


@pytest.fixture
def editable(tmp_path):
    """使用本地真实小模型覆盖写回流程，测试不修改用户模型。"""
    root = tmp_path / "模型"
    model, tokenizer = create_local_model(root)
    return QwenAdapter(model, tokenizer, inspect_model(root, hash_weights=True))


def test_temporary_scope_and_normalized_steering(editable):
    """多条规则按范围生效，引导的输出向量范数与配方一致。"""
    adapter = editable
    baseline = torch.randn(1, 4, 40)
    residual = torch.randn(1, 4, 16)
    adapter.prompt_length = 2
    adapter.edit_rules = validate_rules(adapter, [
        dict(layer_index=0, neuron_index=0, kind="zero", scope="all", value=0),
        dict(layer_index=0, neuron_index=1, kind="add", scope="generated", value=2),
        dict(layer_index=0, neuron_index=2, kind="set", scope="position", token_position=1, value=.5),
        dict(layer_index=0, neuron_index=3, kind="steer", scope="all", value=.25),
    ], "temporary")
    mlp = adapter.backbone.layers[0].mlp
    with torch.inference_mode():
        result = apply_activations(adapter, 0, baseline, [0, 1, 2, 3], residual, mlp)
        assert torch.equal(result[:, :, 0], torch.zeros(1, 4))
        torch.testing.assert_close(result[0, :2, 1], baseline[0, :2, 1])
        torch.testing.assert_close(result[0, 2:, 1], baseline[0, 2:, 1] + 2)
        assert result[0, 1, 2] == .5
        expected = (residual + mlp.down_proj(baseline)).float().norm(dim=-1)[0, 1:].median() * .25
        actual = (result[0, 0, 3] - baseline[0, 0, 3]) * mlp.down_proj.weight[:, 3].norm()
        torch.testing.assert_close(actual, expected)
    assert not torch.equal(result, baseline)
    assert adapter.info["hashes"] == inspect_model(adapter.info["path"], hash_weights=True)["hashes"]


def test_save_reload_and_backup_restore(editable):
    """修改直接落入当前目录，重新加载可见，备份恢复原数值。"""
    adapter = QwenAdapter.load(editable.info["path"], "cpu", "float32")
    before = adapter.parameter(0, "down").detach().clone()
    original_hashes = copy.deepcopy(adapter.info["hashes"])
    rules = validate_rules(adapter, [dict(layer_index=0, neuron_index=3, projection="down", kind="zero", value=0),
        dict(layer_index=0, neuron_index=5, projection="gate", kind="set", dimension=4, value=.625)], "model")
    result = save_current_model(adapter, prepare_weights(adapter, rules))
    reloaded = QwenAdapter.load(adapter.info["path"], "cpu", "float32")
    assert adapter.info["model_revision"] == reloaded.info["model_revision"]
    assert torch.equal(reloaded.parameter(0, "down")[:, 3], torch.zeros(16))
    assert reloaded.parameter(0, "gate")[5, 4].item() == .625
    assert adapter.info["hashes"] != original_hashes
    restore_backup(adapter, result["backup_path"])
    restored = QwenAdapter.load(adapter.info["path"], "cpu", "float32")
    torch.testing.assert_close(restored.parameter(0, "down"), before, rtol=0, atol=0)
    assert adapter.info["hashes"] == original_hashes


def test_invalid_batch_never_partially_applies(editable):
    """第二条规则无效时，第一条也不能污染真实参数。"""
    before = editable.parameter(0, "down").detach().clone()
    with pytest.raises(ProtocolError):
        rules = validate_rules(editable, [dict(layer_index=0, neuron_index=2, kind="zero", value=0),
            dict(layer_index=0, neuron_index=400, kind="zero", value=0)], "model")
        prepare_weights(editable, rules)
    torch.testing.assert_close(editable.parameter(0, "down"), before, rtol=0, atol=0)
    with pytest.raises(ProtocolError):
        validate_rules(editable, [dict(layer_index=0, neuron_index=2, kind="zero", scope="all", value=0)] * 2, "temporary")


def test_interrupted_commit_rolls_back_all_shards(editable, monkeypatch):
    """多分块提交中途失败时恢复全部原文件与内存参数。"""
    import observatory.editing as editing
    adapter = editable
    root = editing.Path(adapter.info["path"])
    (root / "model.safetensors").unlink()
    adapter.model.save_pretrained(root, max_shard_size="1KB")
    adapter.info = inspect_model(root, hash_weights=True)
    initial = copy.deepcopy(adapter.info["hashes"])
    before = adapter.parameter(0, "down").detach().clone()
    rules = validate_rules(adapter, [dict(layer_index=0, neuron_index=3, projection=p, kind="zero", value=0) for p in ("gate", "down")], "model")
    original_replace = editing.os.replace
    count = 0
    def fail_second_shard(source, target):
        """只在第二个权重提交点注入一次故障，恢复路径仍可写入。"""
        nonlocal count
        if "staged" in str(source) and str(target).endswith(".safetensors"):
            count += 1
            if count == 2:
                raise OSError("测试磁盘故障")
        return original_replace(source, target)
    monkeypatch.setattr(editing.os, "replace", fail_second_shard)
    with pytest.raises(OSError):
        save_current_model(adapter, prepare_weights(adapter, rules))
    assert count == 2
    assert {name: digest(root / name) for name in initial} == initial
    torch.testing.assert_close(adapter.parameter(0, "down"), before, rtol=0, atol=0)
    assert not (root / "observatory-save-journal.json").exists()


def test_external_disk_change_rejected(editable):
    """模型加载后被外部改写时不覆盖外部文件。"""
    root = __import__("pathlib").Path(editable.info["path"])
    path = root / "model.safetensors"
    with path.open("ab") as handle:
        handle.write(b"external")
    rules = validate_rules(editable, [dict(layer_index=0, neuron_index=3, kind="zero", value=0)], "model")
    with pytest.raises(ProtocolError, match="变化"):
        save_current_model(editable, prepare_weights(editable, rules))


def test_commit_metadata_failure_restores_memory_and_disk(editable, monkeypatch):
    """内存已更新后提交标记失败时，仍恢复磁盘和全部参数。"""
    import observatory.editing as editing
    initial = copy.deepcopy(editable.info["hashes"])
    before = editable.parameter(0, "down").detach().clone()
    original_write = editing.write_json
    def fail_commit(path, payload):
        """只在提交标记注入故障，允许回滚程序读取原事务。"""
        if payload.get("state") == "committed":
            raise OSError("提交元数据失败")
        return original_write(path, payload)
    monkeypatch.setattr(editing, "write_json", fail_commit)
    rules = validate_rules(editable, [dict(layer_index=0, neuron_index=3, kind="zero", value=0)], "model")
    with pytest.raises(OSError):
        save_current_model(editable, prepare_weights(editable, rules))
    torch.testing.assert_close(editable.parameter(0, "down"), before, rtol=0, atol=0)
    assert inspect_model(editable.info["path"], hash_weights=True)["hashes"] == initial


def test_cpu_edit_preserves_storage_precision(tmp_path):
    """处理器以单精度计算时，保存仍保留脑浮点精度并同步舍入后的内存。"""
    from safetensors import safe_open
    source = tmp_path / "脑浮点模型"
    model, tokenizer = create_local_model(source)
    model.to(torch.bfloat16).save_pretrained(source)
    adapter = QwenAdapter.load(source, "cpu", "float32")
    rules = validate_rules(adapter, [dict(layer_index=0, neuron_index=3, dimension=4,
        projection="down", kind="set", value=.123456)], "model")
    save_current_model(adapter, prepare_weights(adapter, rules))
    rounded = torch.tensor(.123456, dtype=torch.bfloat16).item()
    assert adapter.weight_value(0, 3, "down", 4) == rounded
    with safe_open(str(source / "model.safetensors"), framework="pt", device="cpu") as handle:
        assert handle.get_tensor("model.layers.0.mlp.down_proj.weight").dtype == torch.bfloat16
    loaded = QwenAdapter.load(source, "cpu", "float32")
    assert loaded.weight_value(0, 3, "down", 4) == rounded
