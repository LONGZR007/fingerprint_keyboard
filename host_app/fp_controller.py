"""指纹操作流程控制器。

固件状态机约束（fp_sm.c）：
- 上电默认 FP_VERIFY（常驻 1:N 验证轮询）
- 只有 FP_IDLE 状态才能启动 enroll/delete/clear/read_index
- 因此任何操作流程必须为：fp_cancel →（收到取消确认）→ 执行操作 → fp_verify 切回验证

本模块提供同步流程 API（内部用 QEventLoop，不冻结 GUI），并做防重入。
"""
from PyQt5.QtCore import QObject, pyqtSignal

from parser import parse_fp_line, parse_index_hex, index_to_ids


class FpController(QObject):
    """指纹流程控制。所有方法在主线程调用，同一时刻只允许一个流程。"""

    log = pyqtSignal(str)      # 流程日志（界面显示用）
    step = pyqtSignal(str)     # 录入步骤提示（界面弹提示用）

    CANCEL_TIMEOUT = 6000
    ENROLL_TIMEOUT = 30000     # 固件录入超时 20s，留余量
    OP_TIMEOUT = 15000
    INDEX_TIMEOUT = 20000

    def __init__(self, api, parent=None):
        super().__init__(parent)
        self._api = api
        self._busy = False

    @property
    def busy(self):
        return self._busy

    # ==================================================================
    # 公共流程
    # ==================================================================

    def enroll(self, uid, on_step=None):
        """录入指纹到指定 ID。返回 (ok, message)。"""
        if self._busy:
            return False, "设备忙，请先完成当前操作"
        self._busy = True
        try:
            return self._do_enroll(int(uid), on_step)
        finally:
            self._busy = False

    def delete_fp(self, uid):
        """删除指定 ID 的指纹。返回 (ok, message)。"""
        if self._busy:
            return False, "设备忙，请先完成当前操作"
        self._busy = True
        try:
            return self._do_delete(int(uid))
        finally:
            self._busy = False

    def clear_fp(self):
        """清空全部指纹。返回 (ok, message)。"""
        if self._busy:
            return False, "设备忙，请先完成当前操作"
        self._busy = True
        try:
            return self._do_clear()
        finally:
            self._busy = False

    def read_index(self):
        """读取指纹索引表。返回 (ok, ids_set, message)。"""
        if self._busy:
            return False, set(), "设备忙，请先完成当前操作"
        self._busy = True
        try:
            return self._do_read_index()
        finally:
            self._busy = False

    # ==================================================================
    # 内部实现
    # ==================================================================

    def _cancel_to_idle(self):
        """取消当前操作直到 FP_IDLE。成功返回 True。

        设备在 VERIFY 时：'fp_cancel sent' → '[FP] 已取消'
        设备已在 IDLE 时：'fp idle, nothing to cancel'
        """
        self.log.emit(">>> fp_cancel")
        self._api.send("fp_cancel")
        line = self._api.wait_for(
            lambda l: ("已取消" in l) or ("nothing to cancel" in l),
            timeout_ms=self.CANCEL_TIMEOUT)
        if line is None:
            self.log.emit("!! 取消操作超时")
            return False
        return True

    def _restore_verify(self):
        """切回 1:N 验证状态。"""
        self.log.emit(">>> fp_verify")
        self._api.send("fp_verify")
        self._api.wait_for(
            lambda l: ("fp_verify started" in l) or ("忙" in l),
            timeout_ms=4000)

    def _do_enroll(self, uid, on_step):
        if not self._cancel_to_idle():
            self._restore_verify()
            return False, "取消当前验证状态失败"
        self.log.emit(f">>> fp_enroll {uid}")
        self._api.send(f"fp_enroll {uid}")

        def on_line(line):
            msg = parse_fp_line(line)
            if msg and "步骤" in msg:
                self.step.emit(msg)
                if on_step:
                    on_step(msg)
            if ("注册成功" in line) or ("注册失败" in line) \
                    or ("操作超时" in line) or ("忙" in line) or ("fp busy" in line):
                return True
            return False

        line = self._api.wait_for(on_line, timeout_ms=self.ENROLL_TIMEOUT)
        self._restore_verify()
        if line and "注册成功" in line:
            return True, f"ID {uid} 指纹录入成功"
        reason = line if line else "超时（请重试，注意指纹模块状态）"
        return False, f"ID {uid} 指纹录入失败: {reason}"

    def _do_delete(self, uid):
        if not self._cancel_to_idle():
            self._restore_verify()
            return False, "取消当前验证状态失败"
        self.log.emit(f">>> fp_delete {uid}")
        line = self._api.send_and_wait(
            f"fp_delete {uid}",
            lambda l: ("删除成功" in l) or ("删除失败" in l)
                      or ("操作超时" in l) or ("忙" in l) or ("fp busy" in l),
            timeout_ms=self.OP_TIMEOUT)
        self._restore_verify()
        if line and "删除成功" in line:
            return True, f"ID {uid} 指纹删除成功"
        return False, f"ID {uid} 指纹删除失败: {line or '超时'}"

    def _do_clear(self):
        if not self._cancel_to_idle():
            self._restore_verify()
            return False, "取消当前验证状态失败"
        self.log.emit(">>> fp_clear")
        line = self._api.send_and_wait(
            "fp_clear",
            lambda l: ("清空成功" in l) or ("清空失败" in l)
                      or ("操作超时" in l) or ("忙" in l) or ("fp busy" in l),
            timeout_ms=self.OP_TIMEOUT)
        self._restore_verify()
        if line and "清空成功" in line:
            return True, "全部指纹已清空"
        return False, f"清空指纹失败: {line or '超时'}"

    def _do_read_index(self):
        if not self._cancel_to_idle():
            self._restore_verify()
            return False, set(), "取消当前验证状态失败"
        self.log.emit(">>> fp_read_index")
        hex_rows = []

        def accumulate(line):
            if ("读取失败" in line) or ("操作超时" in line) \
                    or ("忙" in line) or ("fp busy" in line):
                hex_rows.append(None)  # 失败标记
                return True
            h = parse_index_hex(line)
            if h is not None:
                hex_rows.append(h)
                return len(hex_rows) >= 8
            return False

        self._api.send("fp_read_index")
        line = self._api.wait_for(accumulate, timeout_ms=self.INDEX_TIMEOUT)
        self._restore_verify()
        if line is None:
            return False, set(), "读取索引表超时"
        if hex_rows and hex_rows[-1] is None:
            return False, set(), "读取索引表失败"
        flat = [b for row in hex_rows if row for b in row]
        if len(flat) < 7:  # 至少覆盖 56 个 ID，防数据不全
            return False, set(), f"索引表数据不完整（{len(flat)} 字节）"
        ids = index_to_ids(flat[:128])
        return True, ids, f"索引表读取成功，{len(ids)} 个 ID 已录指纹"
