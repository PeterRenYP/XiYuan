"""独立文件浏览、比较、备份与回退验收。"""
import json

import numpy as np
import pytest
from safetensors.numpy import save_file, load_file

from observatory.weightfiles import WeightFiles
from observatory.protocol import ProtocolError


def test_nonfinite_preview_and_large_integer_display(tmp_path):
    """非有限旧值仍可修复；超大整数以精确文本显示并拒绝不安全写入。"""
    path=tmp_path/"edge.safetensors"
    save_file({"nonfinite":np.array([np.nan,np.inf],dtype=np.float32),"integer":np.array([2**63-1],dtype=np.int64)},str(path))
    files=WeightFiles();files.open(path)
    assert files.edit({"name":"nonfinite","values":[1.]})["before"]==[None]
    assert files.slice({"name":"integer","count":1})["values"]==[str(2**63-1)]
    with pytest.raises(ProtocolError,match="精确数值范围"):
        files.edit({"name":"integer","values":[2**53+2]})


def test_file_slice_edit_backup_restore(tmp_path):
    """单文件不依赖模型配置，编辑仅改变指定区间并能完整恢复。"""
    path = tmp_path / "model.safetensors"
    save_file({"a":np.arange(12,dtype=np.float32).reshape(3,4),"b":np.array([5,6],dtype=np.int16)},str(path))
    original = path.read_bytes()
    files = WeightFiles()
    opened = files.open(path)
    assert opened["total"]==2
    assert files.slice({"name":"a","start":3,"count":3})["values"]==[3.,4.,5.]
    payload = {"name":"a","start":3,"values":[.25,.5],"expected_revision":files.revision}
    assert files.edit(payload)["after"]==[.25,.5]
    assert path.read_bytes()==original
    saved=files.edit(payload,True)
    actual=load_file(str(path))
    assert actual["a"].flatten().tolist()==[0,1,2,.25,.5,5,6,7,8,9,10,11]
    assert actual["b"].tolist()==[5,6]
    with pytest.raises(ProtocolError,match="变化"):
        files.edit(payload,True)
    files.restore({"backup":saved["backup"],"expected_revision":files.revision})
    assert path.read_bytes()==original


def test_index_missing_shape_dtype_and_numerical_comparison(tmp_path):
    """比较能区分缺失、形状、精度及数值变化；缺失分片明确失败。"""
    first,second=tmp_path/"one.safetensors",tmp_path/"two.safetensors"
    save_file({"same":np.array([1,2],dtype=np.float32),"shape":np.zeros((2,2),dtype=np.float32),"missing":np.zeros(1,dtype=np.float32)},str(first))
    save_file({"same":np.array([1,3],dtype=np.float16),"shape":np.zeros((1,4),dtype=np.float32)},str(second))
    index=tmp_path/"model.safetensors.index.json"
    index.write_text(json.dumps({"weight_map":{"same":first.name,"shape":first.name,"missing":first.name}}))
    files=WeightFiles();files.open(index)
    rows={r["name"]:r for r in files.compare({"other":str(second)})["rows"]}
    assert rows["same"]["maximum_absolute"]==1 and not rows["same"]["dtype_equal"]
    assert rows["shape"]["status"]=="shape_mismatch"
    assert rows["missing"]["status"]=="missing"
    index.write_text(json.dumps({"weight_map":{"a":"missing.safetensors"}}))
    with pytest.raises(ProtocolError,match="分片"):
        WeightFiles().open(index)


def test_file_nonfinite_and_integer_validation(tmp_path):
    """越界整数、小数与旧版本不能写入；非有限原值明确显示未定义。"""
    path=tmp_path/"small.safetensors"
    save_file({"a":np.array([np.nan,np.inf],dtype=np.float32),"i":np.array([1,2],dtype=np.int8)},str(path))
    files=WeightFiles();files.open(path)
    assert files.slice({"name":"a","count":2})["values"]==[None,None]
    for value in (128,1.5,float("nan")):
        with pytest.raises(ProtocolError):
            files.edit({"name":"i","start":0,"values":[value],"expected_revision":files.revision},True)


def test_nested_shard_backup_restore(tmp_path):
    """子目录同名分片的备份只允许恢复到原分片。"""
    nested=tmp_path/"shards";nested.mkdir();path=nested/"one.safetensors"
    save_file({"a":np.arange(4,dtype=np.float32)},str(path));original=path.read_bytes()
    index=tmp_path/"model.safetensors.index.json";index.write_text(json.dumps({"weight_map":{"a":"shards/one.safetensors"}}))
    files=WeightFiles();files.open(index)
    saved=files.edit({"name":"a","start":0,"values":[7.],"expected_revision":files.revision},True)
    files.restore({"backup":saved["backup"],"expected_revision":files.revision})
    assert path.read_bytes()==original
