"""串口通信层：端口枚举、后台读写线程、按行分帧。"""
import threading
import time

import serial
from serial.tools import list_ports
from PyQt5.QtCore import QThread, pyqtSignal

# 设备 CLI 行结束符
LINE_END = b"\r\n"
# 默认波特率
DEFAULT_BAUDRATES = ["115200", "9600", "19200", "38400", "57600", "230400", "460800", "921600"]


def list_serial_ports():
    """枚举系统串口，返回 [(port, description), ...]"""
    ports = []
    for p in list_ports.comports():
        desc = p.description or ""
        ports.append((p.device, f"{p.device} - {desc}" if desc else p.device))
    return ports


class SerialWorker(QThread):
    """后台串口读写线程。

    - 主线程调用 write_line() 发送命令（内部加 \\r\\n）
    - run() 循环读取，按行分帧后通过 line_received 信号发回主线程
    - 固件会回显收到的命令，回显行通过 echo_received 单独上报
    """

    line_received = pyqtSignal(str)          # 设备输出的一行（不含行尾符）
    echo_received = pyqtSignal(str)          # 固件回显的命令行
    connected = pyqtSignal(str)              # 已连接，参数为端口名
    disconnected = pyqtSignal()              # 已断开
    error_occurred = pyqtSignal(str)         # 错误信息

    def __init__(self, parent=None):
        super().__init__(parent)
        self._serial = None
        self._running = False
        self._lock = None     # 由 open_port 初始化
        self._sent = []       # 最近发送的命令行（用于识别回显）

    # ---------- 公开接口 ----------

    def open_port(self, port, baudrate):
        """打开串口。失败抛异常，成功启动线程。"""
        self._serial = serial.Serial()
        self._serial.port = port
        self._serial.baudrate = int(baudrate)
        self._serial.bytesize = serial.EIGHTBITS
        self._serial.parity = serial.PARITY_NONE
        self._serial.stopbits = serial.STOPBITS_ONE
        self._serial.timeout = 0.2
        self._serial.write_timeout = 1.0
        self._serial.open()
        self._lock = threading.Lock()
        self._running = True
        self.start()

    def close_port(self):
        self._running = False
        if self._serial and self._serial.is_open:
            try:
                self._serial.close()
            except Exception:
                pass
        self.wait(1000)

    def is_connected(self):
        return bool(self._serial and self._serial.is_open)

    def write_line(self, cmd: str):
        """发送一行命令。串口未打开或发送失败返回 False。"""
        if not self.is_connected():
            return False
        try:
            data = cmd.encode("utf-8", errors="replace") + LINE_END
            with self._lock:
                self._serial.write(data)
                self._serial.flush()
            with self._lock:
                self._sent.append(cmd)
                if len(self._sent) > 8:
                    del self._sent[:-8]
            return True
        except Exception as e:
            self.error_occurred.emit(f"发送失败: {e}")
            self._handle_disconnect()
            return False

    # ---------- 线程主体 ----------

    def run(self):
        try:
            self.connected.emit(self._serial.port)
        except Exception as e:
            self.error_occurred.emit(str(e))
            self._running = False
            return

        buf = bytearray()
        while self._running and self._serial and self._serial.is_open:
            try:
                chunk = self._serial.read(256)
            except Exception as e:
                self.error_occurred.emit(f"读取错误: {e}")
                break
            if not chunk:
                continue
            buf.extend(chunk)
            while True:
                idx = buf.find(b"\n")
                if idx < 0:
                    # 行太长强制切分，防止缓冲区无限增长
                    if len(buf) > 2048:
                        idx = len(buf) - 1
                    else:
                        break
                line = bytes(buf[: idx + 1]).decode("utf-8", errors="replace")
                del buf[: idx + 1]
                line = line.replace("\r", "").replace("\n", "").strip()
                if not line:
                    continue
                self._classify_line(line)
        # 线程退出时确保断开
        if self._serial and self._serial.is_open:
            try:
                self._serial.close()
            except Exception:
                pass
        self.disconnected.emit()

    def _classify_line(self, line):
        """判断一行是回显还是设备主动输出。"""
        # 固件回显是逐字节的，但整行内容与发送内容一致
        with self._lock:
            if self._sent and line == self._sent[0]:
                self._sent.pop(0)
                is_echo = True
            else:
                is_echo = False
        if is_echo:
            self.echo_received.emit(line)
        else:
            self.line_received.emit(line)

    def _handle_disconnect(self):
        self._running = False
