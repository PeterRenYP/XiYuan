"""验证本地模型导出重载、协议事件和离线读取闭环。"""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import threading

import jsonschema
import torch
from tokenizers import Tokenizer
from tokenizers.models import WordLevel
from tokenizers.pre_tokenizers import Whitespace
from transformers import PreTrainedTokenizerFast, Qwen2Config, Qwen2ForCausalLM

from observatory.model import QwenAdapter, inspect_model
from observatory.protocol import Emitter, command
from observatory.traces import TraceStore
from observatory.worker import Runtime
from observatory.control import Cancelled


def create_local_model(path, layers=1):
    """建立完全本地的小模型，使用真实分词器与真实权重文件。"""
    torch.manual_seed(8)
    config = Qwen2Config(vocab_size=32, hidden_size=16, intermediate_size=40, num_hidden_layers=layers,
                        num_attention_heads=2, num_key_value_heads=1, max_position_embeddings=128)
    model = Qwen2ForCausalLM(config).eval()
    model.save_pretrained(path)
    backend = Tokenizer(WordLevel({"[UNK]": 0, "[EOS]": 1, "你好": 2, "用户": 3, "模型": 4}, unk_token="[UNK]"))
    backend.pre_tokenizer = Whitespace()
    tokenizer = PreTrainedTokenizerFast(tokenizer_object=backend, unk_token="[UNK]", eos_token="[EOS]", pad_token="[EOS]")
    tokenizer.chat_template = "{% for message in messages %}{{ message['role'] + ' ' + message['content'] + ' ' }}{% endfor %}assistant "
    tokenizer.save_pretrained(path)
    return model, tokenizer


def test_export_reload_and_offline_trace(tmp_path):
    """修改后导出重载数值一致，原权重哈希不变，离线记录可读。"""
    source, destination = tmp_path / "原模型", tmp_path / "导出模型"
    model, tokenizer = create_local_model(source)
    initial = inspect_model(source, hash_weights=True)
    adapter = QwenAdapter(model, tokenizer, initial)
    adapter.patch(dict(layer_index=0, neuron_index=39, dimension=15, kind="gate", value=.625,
                       expected_weights_revision="w0"))
    stream = io.StringIO()
    runtime = Runtime(Emitter("test", stream))
    runtime.adapter = adapter
    runtime.export_model(command("model.export", {"destination": str(destination)}))
    loaded = QwenAdapter.load(destination, "cpu", "float32")
    assert loaded.weight_value(0, 39, "gate", 15) == .625
    assert inspect_model(source, hash_weights=True)["hashes"] == initial["hashes"]
    inputs = torch.tensor([[2, 3, 4]])
    with torch.inference_mode():
        old = adapter.forward(inputs, [0, 1, 2], lambda layer, op, fn, values: fn())
        new = loaded.forward(inputs, [0, 1, 2], lambda layer, op, fn, values: fn())
        torch.testing.assert_close(old, new, atol=1e-6, rtol=1e-5)
    runtime.run(command("run.start", {"messages": [{"role": "user", "content": "你好"}], "mode": "observe",
        "generation": {"temperature": 0, "max_new_tokens": 2, "seed": 42}, "trace_root": str(tmp_path / "记录"),
        "capture": {"policy": "standard", "disk_budget_bytes": 32 * 1024**2}}))
    directory = runtime.store.root
    before = runtime.query(dict(branch_id="base", forward_id="f0", layer_index=0, token_position=0, neuron_index=39, operation="mlp", head=1))
    assert before["neuron"]["rank"]
    runtime.store.close()
    runtime.store = TraceStore(directory, readonly=True)
    runtime.adapter = None
    after = runtime.query(dict(branch_id="base", forward_id="f0", layer_index=0, token_position=0, neuron_index=39, operation="mlp", head=1))
    assert before["neuron"] == after["neuron"]
    assert after["probabilities"][0]["text"] == before["probabilities"][0]["text"]
    class WrongTokenizer:
        """历史记录禁止使用另一模型的词表解码。"""

        def decode(self, ids):
            """错误混用词表时立即暴露回归。"""
            raise AssertionError("跨模型读取不能调用当前分词器")

    loaded.info = dict(loaded.info, model_revision="other-model")
    loaded.weights_revision = "w1"
    loaded.tokenizer = WrongTokenizer()
    runtime.adapter = loaded
    cross_model = runtime.query(dict(branch_id="base", forward_id="f0", layer_index=0,
        token_position=0, neuron_index=39, operation="mlp", head=1, lens=True))
    assert "lens" not in cross_model and "weight" not in cross_model
    assert cross_model["probabilities"] == after["probabilities"]
    schema = json.loads((Path(__file__).resolve().parent.parent / "开发附件/protocol-v1.schema.json").read_text(encoding="utf-8"))
    for line in stream.getvalue().splitlines():
        message = json.loads(line)
        if message["name"] in ("operation.completed", "run.finished"):
            jsonschema.validate(message, schema)
    runtime.handle(command("shutdown"))
    runtime.thread.join(5)


def test_loading_cancel_and_utf8_pipe(tmp_path):
    """加载可协作取消；即使系统默认 GBK，工作进程协议仍使用 UTF-8。"""
    source = tmp_path / "取消加载模型"
    create_local_model(source)
    cancel = threading.Event()
    cancel.set()
    try:
        QwenAdapter.load(source, "cpu", "auto", cancel=cancel)
    except Cancelled:
        pass
    else:
        raise AssertionError("已经取消的加载不应成功")
    root = Path(__file__).resolve().parent.parent
    wire = json.dumps(command("不存在的命令"), ensure_ascii=False).encode("utf-8") + b"\n"
    completed = subprocess.run([sys.executable, str(root / "main.py"), "--worker"], input=wire, capture_output=True,
                               env=os.environ | {"PYTHONIOENCODING": "gbk"}, timeout=20)
    messages = [json.loads(line) for line in completed.stdout.decode("utf-8").splitlines()]
    failure = next(m for m in messages if m["kind"] == "error")
    assert failure["payload"]["message"] == "未知运行命令"


def test_invalid_switch_keeps_loaded_model(tmp_path):
    """错误目录预检查失败时保留现有模型和运行身份。"""
    model, tokenizer = create_local_model(tmp_path / "有效模型")
    adapter = QwenAdapter(model, tokenizer)
    stream = io.StringIO()
    runtime = Runtime(Emitter("switch-test", stream))
    runtime.adapter = adapter
    runtime.handle(command("model.load", {"path": str(tmp_path / "无效目录"), "device": "cpu"}))
    runtime.tasks.join()
    assert runtime.adapter is adapter
    assert not runtime.loading and not runtime.busy
    frames = [json.loads(line) for line in stream.getvalue().splitlines()]
    assert any(frame["kind"] == "error" and frame["payload"]["code"] == "INVALID_MODEL" for frame in frames)
    assert not any(frame["name"] == "model.unloaded" for frame in frames)
    runtime.handle(command("shutdown"))
    runtime.thread.join(5)
