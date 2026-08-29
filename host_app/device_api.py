"""设备协议层：CLI 命令封装、输出行解析、伪同步等待。

固件输出格式（来自 combo_keyboard/APP 源码）：
  [FP] 注册成功, ID=%u
  [FP] 步骤: step=%u, n=%u (提示)
  [FP] 注册失败, code=0x%02X
  [FP] 删除成功 / 删除失败, code=0x%02X
  [FP] 已取消 / 操作超时 / 忙, 请先完成当前操作
  [FP] 索引表读取成功, 共 128 字节:   (后跟 8 行每行 16 字节 hex)
    fp_enroll started, ID=%u / fp busy, cannot start enroll
  user set ok, id=%u name="%s" ch=%s data="%s"
  id=%u name="%s" ch=%s data="%s" 或 id=%u empty
  user del ok, id=%u / user del FAILED (id=%u)
"""
from PyQt5.QtCore import QObject, pyqtSignal, QEventLoop, QTimer

from parser import (MAX_USERS, CHANNELS, UserInfo,  # noqa: F401
                    parse_user_get, parse_user_list_line)


# ======================================================================
# 设备 API
# ======================================================================

class DeviceApi(QObject):
    """面向上层（控制器/界面）的异步命令接口。

    注意：所有方法在主线程调用；wait_for/send_and_wait 通过 QEventLoop
    处理事件，不会冻结 GUI，但同一时刻只能有一个等待者（由流程控制器
    保证串行）。
    """

    line_received = pyqtSignal(str)     # 设备主动输出的一行（去回显）
    echo_received = pyqtSignal(str)     # 固件回显的命令行
    raw_sent = pyqtSignal(str)          # 本机发出的命令

    def __init__(self, worker, parent=None):
        super().__init__(parent)
        self._worker = worker
        worker.line_received.connect(self.line_received)
        worker.echo_received.connect(self.echo_received)

    @property
    def is_connected(self):
        return self._worker.is_connected()

    # ---------- 发送 ----------

    def send(self, cmd):
        """发送一行命令。返回是否成功。"""
        ok = self._worker.write_line(cmd)
        if ok:
            self.raw_sent.emit(cmd)
        return ok

    # ---------- 伪同步等待 ----------

    def wait_for(self, predicate, timeout_ms=10000, collect=None):
        """阻塞直到某行满足 predicate，返回该行；超时返回 None。

        predicate: callable(str) -> bool
        collect: 可选 list，等待期间收到的所有行都会追加进去
        """
        result = []
        loop = QEventLoop()
        timer = QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(loop.quit)

        def on_line(line):
            if collect is not None:
                collect.append(line)
            if predicate(line):
                result.append(line)
                loop.quit()

        self.line_received.connect(on_line)
        timer.start(timeout_ms)
        loop.exec_()
        self.line_received.disconnect(on_line)
        timer.stop()
        return result[0] if result else None

    def send_and_wait(self, cmd, predicate, timeout_ms=10000, collect=None):
        """发送命令并等待满足 predicate 的输出行。"""
        self.send(cmd)
        return self.wait_for(predicate, timeout_ms, collect)

    def read_user_list(self, timeout_ms=5000):
        """发送 'user list' 一次性读取全部用户。

        固件输出：表头 → 若干数据行 → '  total: N user(s)'。
        返回 {uid: UserInfo}；超时或响应异常返回 None。
        """
        lines = []
        line = self.send_and_wait(
            "user list",
            lambda l: "total:" in l or "invalid" in l or "FAILED" in l,
            timeout_ms, collect=lines)
        if line is None or "total:" not in line:
            return None
        users = {}
        for l in lines:
            info = parse_user_list_line(l)
            if info is not None:
                users[info.uid] = info
        return users
