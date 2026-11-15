"""真实计算边界的放行、模式切换和取消。"""
from __future__ import annotations

import threading
import uuid
from collections import deque

from .protocol import ProtocolError


class Cancelled(Exception):
    """协作取消，不作为运行失败。"""


class StepController:
    """在操作执行前等待；重复请求和旧边界不得多放行。"""

    def __init__(self, mode="observe", notify=None):
        self.condition = threading.Condition()
        self.mode = mode
        self.notify = notify or (lambda *args: None)
        self.cancelled = False
        self.waiting = False
        self.boundary_id = None
        self.revision = 0
        self.released = False
        self.requests = deque(maxlen=4096)
        self.executed = 0
        self.replay_remaining = 0
        self.save_callback = None
        self.breakpoints = []

    def configure(self, points):
        """安装已校验的断点快照，命中一次后移除，继续不会卡在同一条件。"""
        with self.condition:
            self.breakpoints = [dict(point) for point in points]

    def matches(self, point, context, boundary):
        """层、算子和生成步采用零起始索引，空目标表示不限。"""
        return point.get("boundary", "before") == boundary and all(
            point.get(key) is None or point[key] == context.get(key)
            for key in ("layer_index", "operation_id", "generation_step"))

    def after(self, context, value):
        """条件监视在结果已采集后暂停，条件值使用所选通道绝对峰值或 Token 概率。"""
        import math
        with self.condition:
            if self.cancelled:
                raise Cancelled()
            for point in list(self.breakpoints):
                if not self.matches(point, context, "after"):
                    continue
                index = point["index"]
                result = float(value[..., index].detach().abs().max()) if point["watch"] == "channel" else float(value[index])
                if not math.isfinite(result):
                    continue
                threshold = point["threshold"]
                hit = {"gt": result > threshold, "ge": result >= threshold, "lt": result < threshold, "le": result <= threshold}[point["comparison"]]
                if not hit:
                    continue
                self.breakpoints.remove(point)
                self.mode = "step"
                self.revision += 1
                self.boundary_id = uuid.uuid4().hex
                self.waiting, self.released = True, False
                self.notify("step.waiting", dict(context, boundary="after", observed_value=result,
                    boundary_id=self.boundary_id, state_revision=self.revision, executed=self.executed))
                while self.mode == "step" and not self.released and not self.cancelled:
                    self.condition.wait(.25)
                self.waiting = False
                if self.cancelled:
                    raise Cancelled()
                break

    def checkpoint(self, context):
        """到达下一边界后才发布等待事件；条件等待时仍可接收命令。"""
        with self.condition:
            if self.cancelled:
                raise Cancelled()
            if self.replay_remaining:
                self.replay_remaining -= 1
                self.executed += 1
                return
            self.revision += 1
            for point in list(self.breakpoints):
                if self.matches(point, context, "before"):
                    self.breakpoints.remove(point)
                    self.mode = "step"
                    context = dict(context, boundary="before", breakpoint=True)
                    break
            self.boundary_id = uuid.uuid4().hex
            self.released = False
            self.waiting = self.mode == "step"
            if self.waiting:
                self.notify("step.waiting", dict(boundary_id=self.boundary_id,
                            state_revision=self.revision, executed=self.executed, **context))
            if self.save_callback:
                callback, self.save_callback = self.save_callback, None
                callback(self.executed)
            while self.mode == "step" and not self.released and not self.cancelled:
                self.condition.wait(.25)
            self.waiting = False
            if self.cancelled:
                raise Cancelled()
            self.executed += 1

    def save_at_boundary(self, callback):
        """保存请求在稳定运算边界执行；已暂停时可直接保存，不释放运算。"""
        with self.condition:
            self.mode = "step"
            if self.waiting:
                callback(self.executed)
            else:
                self.save_callback = callback
            self.condition.notify_all()

    def release(self, request_id, boundary_id, revision):
        """一次请求只能释放当前等待中的一个边界。"""
        with self.condition:
            if request_id in self.requests:
                return {"duplicate": True}
            if self.mode != "step" or not self.waiting or self.released:
                raise ProtocolError("NOT_WAITING", "当前没有等待放行的计算")
            if boundary_id != self.boundary_id or revision != self.revision:
                raise ProtocolError("STALE_BOUNDARY", "计算边界已变化，请等待界面更新")
            self.requests.append(request_id)
            self.released = True
            self.condition.notify_all()
            return {"accepted": True}

    def set_mode(self, mode):
        """切换策略不清除上下文；连续模式会释放当前等待。"""
        if mode not in ("normal", "observe", "step"):
            raise ProtocolError("INVALID_MODE", "运行模式无效")
        with self.condition:
            self.mode = mode
            self.condition.notify_all()
            return {"mode": mode, "state_revision": self.revision}

    def cancel(self):
        """唤醒等待线程，使关闭和取消不会死锁。"""
        with self.condition:
            self.cancelled = True
            self.condition.notify_all()
