"""独立 Safetensors 浏览与事务编辑：不加载推理模型，按需映射有界切片。"""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import uuid

import numpy as np

from .protocol import ProtocolError


DTYPES = {"F64": "<f8", "F32": "<f4", "F16": "<f2", "BF16": "<u2", "I64": "<i8", "I32": "<i4",
    "I16": "<i2", "I8": "i1", "U64": "<u8", "U32": "<u4", "U16": "<u2", "U8": "u1", "BOOL": "?"}
NAMES = {"F64": "64 位浮点", "F32": "32 位浮点", "F16": "16 位浮点", "BF16": "脑浮点 16 位",
    "I64": "64 位有符号整数", "I32": "32 位有符号整数", "I16": "16 位有符号整数", "I8": "8 位有符号整数",
    "U64": "64 位无符号整数", "U32": "32 位无符号整数", "U16": "16 位无符号整数", "U8": "8 位无符号整数", "BOOL": "布尔值"}


def header(path):
    """只读文件头并验证偏移，拒绝损坏权重及超大头部。"""
    path = Path(path)
    with path.open("rb") as stream:
        prefix = stream.read(8)
        if len(prefix) != 8:
            raise ProtocolError("INVALID_WEIGHT_FILE", "权重文件头不完整")
        length = struct.unpack("<Q", prefix)[0]
        if not 2 <= length <= 64*1024**2 or length+8 > path.stat().st_size:
            raise ProtocolError("INVALID_WEIGHT_FILE", "权重文件头长度无效")
        raw = stream.read(length)
    data = json.loads(raw)
    tensors = {}
    for name, item in data.items():
        if name == "__metadata__":
            continue
        shape, offsets = item.get("shape"), item.get("data_offsets")
        if not isinstance(shape, list) or any(type(v) is not int or v < 0 for v in shape) or not isinstance(offsets,list) or len(offsets)!=2:
            raise ProtocolError("INVALID_WEIGHT_FILE", "张量形状或数据偏移无效："+name)
        start, stop = offsets
        if type(start) is not int or type(stop) is not int or not 0 <= start <= stop <= path.stat().st_size-length-8:
            raise ProtocolError("INVALID_WEIGHT_FILE", "张量数据越界："+name)
        if item["dtype"] in DTYPES and stop-start != math.prod(shape)*np.dtype(DTYPES[item["dtype"]]).itemsize:
            raise ProtocolError("INVALID_WEIGHT_FILE", "张量大小与形状不一致："+name)
        tensors[name] = item | {"offset": length+8+start, "count": math.prod(shape), "file": str(path)}
    ranges = sorted((v["data_offsets"][0], v["data_offsets"][1]) for v in tensors.values() if v["count"])
    if any(a[1] > b[0] for a,b in zip(ranges,ranges[1:])):
        raise ProtocolError("INVALID_WEIGHT_FILE", "张量数据区间重叠")
    return tensors, hashlib.sha256(raw).hexdigest()


class WeightFiles:
    """一个独立文件工作区，不提供项目推理、规则或调试入口。"""

    def __init__(self):
        """文件版本只在打开、提交或恢复后更新，旧预览必须重新获取。"""
        self.path = None
        self.tensors = {}
        self.stamps = {}
        self.revision = None
        self.last_backup = None

    def open(self, path):
        """单文件和分片索引共用目录边界校验，缺失分片给出实际文件名。"""
        path = Path(path).resolve()
        if not path.is_file():
            raise ProtocolError("FILE_NOT_FOUND", "文件不存在："+str(path))
        tensors, files = {}, {path}
        if path.name.endswith(".index.json"):
            data = json.loads(path.read_text(encoding="utf-8"))
            mapping = data.get("weight_map")
            if not isinstance(mapping,dict) or not mapping:
                raise ProtocolError("INVALID_INDEX", "分片索引缺少有效的权重映射")
            for shard in sorted(set(mapping.values())):
                source = (path.parent / shard).resolve()
                if not source.is_relative_to(path.parent) or not source.is_file():
                    raise ProtocolError("MISSING_SHARD", "缺失或无效分片："+str(shard))
                entries, _ = header(source)
                files.add(source)
                for name, filename in mapping.items():
                    if filename == shard:
                        if name not in entries:
                            raise ProtocolError("INVALID_INDEX", "分片缺少张量："+name)
                        tensors[name] = entries[name]
        else:
            if path.suffix.lower() != ".safetensors":
                raise ProtocolError("INVALID_FORMAT", "请选择 Safetensors 文件或分片索引")
            tensors, _ = header(path)
        stamps = {str(p): self.stamp(p) for p in files}
        self.path, self.tensors, self.stamps = path, tensors, stamps
        self.revision = hashlib.sha256(json.dumps(stamps, sort_keys=True).encode()).hexdigest()
        return self.list()

    @staticmethod
    def stamp(path):
        """文件版本含高精度修改时间、大小和文件标识，写前再次核对。"""
        stat = Path(path).stat()
        return [stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns, stat.st_ino]

    def check(self, revision=None):
        """任何分片变化都使旧切片与预览失效。"""
        if self.path is None:
            raise ProtocolError("NO_FILE", "请先打开权重文件")
        if revision is not None and revision != self.revision or any(self.stamp(p)!=value for p,value in self.stamps.items()):
            raise ProtocolError("STALE_FILE", "权重文件已变化，请重新打开并预览")

    def list(self, query="", offset=0, limit=100):
        """张量名称分页搜索，不将权重载入内存。"""
        self.check()
        offset, limit = int(offset), int(limit)
        if offset<0 or not 1<=limit<=1000:
            raise ProtocolError("INVALID_PAGE", "分页范围无效")
        names = sorted(name for name in self.tensors if query.casefold() in name.casefold())
        rows = [{"name": name, "shape": self.tensors[name]["shape"], "dtype": self.tensors[name]["dtype"],
            "dtype_name": NAMES.get(self.tensors[name]["dtype"], "未适配的精度"), "count": self.tensors[name]["count"],
            "editable": self.tensors[name]["dtype"] in DTYPES} for name in names[offset:offset+limit]]
        return {"path": str(self.path), "revision": self.revision, "total": len(names), "rows": rows, "last_backup": self.last_backup}

    def values(self, name, start, count):
        """映射指定连续区间，脑浮点按原位模式解码，不需要初始化 GPU。"""
        self.check()
        item = self.tensors.get(name)
        if item is None:
            raise ProtocolError("MISSING_TENSOR", "张量不存在："+name)
        if item["dtype"] not in DTYPES:
            raise ProtocolError("UNSUPPORTED_DTYPE", "该精度尚不支持切片和修改："+item["dtype"])
        start, count = int(start), int(count)
        if not 0 <= start <= item["count"] or not 0 <= count <= 1048576 or start+count>item["count"]:
            raise ProtocolError("INVALID_SLICE", "切片范围越界或超过单次上限")
        if count == 0:
            return np.array([], dtype=np.float64)
        dtype = np.dtype(DTYPES[item["dtype"]])
        mapped = np.memmap(item["file"], mode="r", dtype=dtype, offset=item["offset"]+start*dtype.itemsize, shape=(count,))
        values = np.array(mapped)
        del mapped
        if item["dtype"] == "BF16":
            values = (values.astype(np.uint32)<<16).view(np.float32)
        return values

    def slice(self, payload):
        """浏览切片返回真实坐标及有效性，不把非有限数伪装为零。"""
        name = payload["name"]
        start, count = int(payload.get("start",0)), int(payload.get("count",64))
        if count > 65536:
            raise ProtocolError("INVALID_SLICE", "界面与 MCP 单次最多读取 65536 项")
        values = self.values(name,start,count)
        item = self.tensors[name]
        finite = np.isfinite(values)
        return {"name": name, "shape": item["shape"], "dtype": item["dtype"], "dtype_name": NAMES[item["dtype"]],
            "start": start, "total": item["count"], "revision": self.revision,
            "values": [str(v.item()) if good and values.dtype.kind in "iu" and abs(int(v)) > 2**53-1 else v.item() if good else None for v,good in zip(values,finite)], "nonfinite": int((~finite).sum())}

    def edit(self, payload, apply=False):
        """修改有界区间，完整备份后原子替换；共享文件不改变其他字节。"""
        self.check(payload.get("expected_revision"))
        name = payload["name"]
        values = payload.get("values")
        if not isinstance(values,list) or not 1<=len(values)<=65536 or any(type(v) not in (int,float,bool) or not math.isfinite(v) for v in values):
            raise ProtocolError("INVALID_VALUES", "请提供 1 至 65536 个有限数值")
        start = int(payload.get("start",0))
        before = self.values(name,start,len(values))
        item = self.tensors[name]
        dtype = np.dtype(DTYPES[item["dtype"]])
        if item["dtype"] == "BOOL" and any(v not in (0,1,False,True) for v in values):
            raise ProtocolError("INVALID_VALUES", "布尔权重只接受 0 或 1")
        if dtype.kind in "iu" and item["dtype"]!="BF16":
            bounds = np.iinfo(dtype)
            if any(int(v)!=v or not bounds.min<=v<=bounds.max for v in values):
                raise ProtocolError("VALUE_OVERFLOW", "数值超出整数精度或包含小数")
            if any(abs(int(v)) > 2**53-1 for v in values):
                raise ProtocolError("UNSAFE_INTEGER", "此整数超出界面和 MCP 的精确数值范围，暂不支持修改")
        with np.errstate(over="ignore",invalid="ignore"):
            converted = np.array(values,dtype=np.float32 if item["dtype"]=="BF16" else dtype)
            if item["dtype"]=="BF16":
                bits = converted.view(np.uint32)
                converted = ((bits + 0x7fff + ((bits>>16)&1))>>16).astype(np.uint16)
                displayed = (converted.astype(np.uint32)<<16).view(np.float32)
            else:
                displayed = converted
        if not np.isfinite(displayed).all():
            raise ProtocolError("VALUE_OVERFLOW", "数值超出目标精度，未写入文件")
        result = {"name":name,"start":start,"count":len(values),"before":[v.item() if np.isfinite(v) else None for v in before[:64]],"after":displayed[:64].tolist(),
            "preview_limited":len(values)>64,"dtype_name":NAMES[item["dtype"]],"revision":self.revision}
        if not apply:
            return result
        if not payload.get("expected_revision"):
            raise ProtocolError("MISSING_REVISION", "写入必须携带预览时的文件版本")
        source = Path(item["file"])
        backup_dir = source.parent / "xiyuan-backups"
        backup_dir.mkdir(exist_ok=True)
        backup = backup_dir / (source.name+"."+uuid.uuid4().hex+".bak")
        staged = source.with_name(source.name+"."+uuid.uuid4().hex+".tmp")
        try:
            shutil.copy2(source,backup)
            self.check(payload["expected_revision"])
            shutil.copy2(source,staged)
            with staged.open("r+b") as stream:
                stream.seek(item["offset"]+start*dtype.itemsize)
                stream.write(converted.tobytes())
                stream.flush();os.fsync(stream.fileno())
            header(staged)
            self.check(payload["expected_revision"])
            os.replace(staged,source)
        finally:
            staged.unlink(missing_ok=True)
        self.last_backup = str(backup)
        self.open(self.path)
        return result | {"revision": self.revision, "backup": str(backup), "saved": True}

    def compare(self, payload):
        """按块比较同名张量，区分缺失、形状、精度及非有限值。"""
        self.check()
        other = WeightFiles()
        other.open(payload["other"])
        names = [payload["name"]] if payload.get("name") else sorted(set(self.tensors)|set(other.tensors))
        rows = []
        atol,rtol = float(payload.get("atol",1e-5)),float(payload.get("rtol",1e-4))
        if not all(math.isfinite(v) and v>=0 for v in (atol,rtol)):
            raise ProtocolError("INVALID_TOLERANCE", "容差须为非负有限数")
        for name in names:
            a,b = self.tensors.get(name),other.tensors.get(name)
            row = {"name":name,"calculation_dtype":"float64","atol":atol,"rtol":rtol}
            if a is None or b is None:
                rows.append(row|{"status":"missing","reason":"当前文件缺失" if a is None else "对照文件缺失"});continue
            row.update(shape=a["shape"],other_shape=b["shape"],dtype=a["dtype"],other_dtype=b["dtype"],dtype_equal=a["dtype"]==b["dtype"])
            if a["shape"]!=b["shape"]:
                rows.append(row|{"status":"shape_mismatch","reason":"形状不匹配，无法逐位置对齐"});continue
            if a["dtype"] not in DTYPES or b["dtype"] not in DTYPES:
                rows.append(row|{"status":"unsupported","reason":"精度尚未适配数值比较"});continue
            maximum,total,valid,notclose,nonfinite = 0.,0.,0,0,0
            for start in range(0,a["count"],1048576):
                count=min(1048576,a["count"]-start)
                x,y=self.values(name,start,count).astype(np.float64),other.values(name,start,count).astype(np.float64)
                finite=np.isfinite(x)&np.isfinite(y);nonfinite+=int((~finite).sum())
                delta=np.abs(x[finite]-y[finite]);valid+=delta.size
                if delta.size:maximum=max(maximum,float(delta.max()));total+=float(delta.sum())
                notclose+=int((~np.isclose(x,y,atol=atol,rtol=rtol,equal_nan=False)).sum())
            rows.append(row|{"status":"compared","maximum_absolute":maximum if valid else None,"mean_absolute":total/valid if valid else None,
                "outside_tolerance":notclose,"valid":valid,"nonfinite":nonfinite})
        return {"rows":rows,"revision":self.revision,"other_revision":other.revision,"path":str(self.path),"other":str(other.path)}

    def restore(self, payload):
        """恢复仅接受本文件相邻备份，恢复前也备份当前文件。"""
        self.check(payload.get("expected_revision"))
        if not payload.get("expected_revision"):
            raise ProtocolError("MISSING_REVISION", "恢复必须携带当前文件版本")
        backup=Path(payload["backup"]).resolve()
        # 分片可以位于索引的子目录，备份归属按实际分片父目录精确匹配。
        matching=[Path(v["file"]) for v in self.tensors.values()
            if backup.parent == Path(v["file"]).parent / "xiyuan-backups" and backup.name.startswith(Path(v["file"]).name+".")]
        if not matching or not backup.is_file():
            raise ProtocolError("INVALID_BACKUP", "请选择当前权重分片目录内的析元备份")
        restored,_=header(backup)
        source=matching[0]
        directory=source.parent / "xiyuan-backups"
        current,_=header(source)
        if set(restored)!=set(current) or any(restored[n]["shape"]!=current[n]["shape"] or restored[n]["dtype"]!=current[n]["dtype"] for n in current):
            raise ProtocolError("INVALID_BACKUP", "备份张量目录不匹配")
        previous=directory/(source.name+"."+uuid.uuid4().hex+".bak")
        staged=source.with_name(source.name+"."+uuid.uuid4().hex+".tmp")
        try:
            shutil.copy2(source,previous);shutil.copy2(backup,staged)
            with staged.open("r+b") as stream:stream.flush();os.fsync(stream.fileno())
            self.check(payload["expected_revision"]);os.replace(staged,source)
        finally:
            staged.unlink(missing_ok=True)
        self.last_backup=str(previous)
        return self.open(self.path)|{"restored":True}

    def dispatch(self, name, payload):
        """界面与 MCP 使用相同只读和事务入口，拒绝所有项目调试命令。"""
        if name=="file.open":return self.open(payload["path"])
        if name=="file.list":return self.list(payload.get("query",""),payload.get("offset",0),payload.get("limit",100))
        if name=="file.slice":return self.slice(payload)
        if name in ("file.preview","file.apply"):return self.edit(payload,name=="file.apply")
        if name=="file.compare":return self.compare(payload)
        if name=="file.restore":return self.restore(payload)
        raise ProtocolError("FILE_MODE_ONLY", "独立文件模式仅支持权重浏览、编辑、比较和恢复")


def main():
    """独立进程按行处理文件请求，不导入推理运行器。"""
    import sys
    from .protocol import Emitter,parse_command
    emitter=Emitter("weight-files")
    workspace=WeightFiles()
    for line in sys.stdin:
        request=None
        try:
            request=parse_command(line)
            if request["name"]=="shutdown":break
            result=workspace.dispatch(request["name"],request["payload"])
            emitter.send("file.result",result,request,"result")
        except Exception as exc:
            error=exc if isinstance(exc,ProtocolError) else ProtocolError("FILE_ERROR","文件操作失败："+str(exc))
            emitter.error(error,request)
