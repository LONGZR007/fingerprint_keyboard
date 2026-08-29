"""指纹键盘 PyQt5 上位机主窗口。"""
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QComboBox, QPushButton, QLineEdit, QPlainTextEdit,
    QTableWidget, QTableWidgetItem, QHeaderView, QAbstractItemView,
    QToolButton, QDialog, QDialogButtonBox, QFormLayout, QMessageBox,
    QGroupBox,
)

from serial_client import SerialWorker, list_serial_ports, DEFAULT_BAUDRATES
from parser import MAX_USERS, CHANNELS, UserInfo, mask_passwords
from device_api import DeviceApi
from fp_controller import FpController


class PasswordCell(QWidget):
    """密码单元格：只读输入框默认显示 *，小眼睛按钮切换明文。"""

    def __init__(self, text="", parent=None):
        super().__init__(parent)
        self._edit = QLineEdit(text, self)
        self._edit.setReadOnly(True)
        self._edit.setEchoMode(QLineEdit.Password)
        self._edit.setFrame(False)
        self._btn = QToolButton(self)
        self._btn.setText("显示")
        self._btn.setCheckable(True)
        self._btn.setToolTip("点击显示/隐藏明文")
        self._btn.toggled.connect(self._toggle)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(4, 0, 4, 0)
        lay.setSpacing(2)
        lay.addWidget(self._edit, 1)
        lay.addWidget(self._btn)

    def _toggle(self, show):
        self._edit.setEchoMode(QLineEdit.Normal if show else QLineEdit.Password)
        self._btn.setText("隐藏" if show else "显示")

    def set_text(self, text):
        self._edit.setText(text)

    def text(self):
        return self._edit.text()

    def reset_mask(self):
        """恢复为掩码显示（如初始化刷新后）。"""
        self._btn.setChecked(False)
        self._edit.setEchoMode(QLineEdit.Password)


class UserDialog(QDialog):
    """设置/修改用户信息对话框。"""

    def __init__(self, uid, name="", channel="both", data="", parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"设置用户信息 - ID {uid}")
        self.setMinimumWidth(420)

        form = QFormLayout()
        id_label = QLabel(str(uid))
        self.name_edit = QLineEdit(name)
        self.name_edit.setMaxLength(15)
        self.ch_combo = QComboBox()
        self.ch_combo.addItems(list(CHANNELS))
        if channel in CHANNELS:
            self.ch_combo.setCurrentText(channel)
        self.data_edit = QLineEdit(data)
        self.data_edit.setEchoMode(QLineEdit.Password)
        self.data_edit.setMaxLength(110)
        self.data_eye = QToolButton()
        self.data_eye.setText("显示")
        self.data_eye.setCheckable(True)
        self.data_eye.toggled.connect(
            lambda show: self.data_edit.setEchoMode(
                QLineEdit.Normal if show else QLineEdit.Password))
        data_row = QWidget()
        dl = QHBoxLayout(data_row)
        dl.setContentsMargins(0, 0, 0, 0)
        dl.addWidget(self.data_edit, 1)
        dl.addWidget(self.data_eye)

        form.addRow("ID:", id_label)
        form.addRow("用户名:", self.name_edit)
        form.addRow("发送通道:", self.ch_combo)
        form.addRow("用户数据(密码):", data_row)

        hint = QLabel("提示：用户名 ≤15 字节；用户数据 ≤110 字节且不能包含空格。")
        hint.setWordWrap(True)
        hint.setStyleSheet("color: gray;")

        btns = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)

        root = QVBoxLayout(self)
        root.addLayout(form)
        root.addWidget(hint)
        root.addWidget(btns)

    def values(self):
        return (self.name_edit.text().strip(),
                self.ch_combo.currentText(),
                self.data_edit.text())


class MainWindow(QMainWindow):
    """上位机主窗口。"""

    def __init__(self):
        super().__init__()
        self.setWindowTitle("指纹键盘上位机")
        self.resize(1080, 720)

        self._worker = SerialWorker(self)
        self._api = DeviceApi(self._worker)
        self._fp = FpController(self._api)

        self._connected = False
        self._busy_ui = False          # 初始化或流程执行中，禁用操作按钮
        self._users = [UserInfo(i) for i in range(MAX_USERS)]
        self._fp_ids = set()

        self._build_ui()
        self._bind_signals()
        self.refresh_ports()

        self.statusBar().showMessage("未连接")

    # ==================================================================
    # UI 构建
    # ==================================================================

    def _build_ui(self):
        central = QWidget()
        root = QVBoxLayout(central)

        # ---------- 连接区 ----------
        conn_box = QGroupBox("连接设置")
        conn_lay = QHBoxLayout(conn_box)
        conn_lay.addWidget(QLabel("串口:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(220)
        self.refresh_btn = QPushButton("刷新")
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(DEFAULT_BAUDRATES)
        self.baud_combo.setCurrentText("115200")
        self.connect_btn = QPushButton("连接")
        self.bond_btn = QPushButton("解除BLE配对")
        self.bond_btn.setEnabled(False)
        self.bond_btn.setToolTip("发送 bond clear：解除所有 BLE 配对，重新进入可配对广播")
        conn_lay.addWidget(self.port_combo)
        conn_lay.addWidget(self.refresh_btn)
        conn_lay.addSpacing(16)
        conn_lay.addWidget(QLabel("波特率:"))
        conn_lay.addWidget(self.baud_combo)
        conn_lay.addSpacing(16)
        conn_lay.addWidget(self.connect_btn)
        conn_lay.addStretch(1)
        conn_lay.addWidget(self.bond_btn)
        root.addWidget(conn_box)

        # ---------- 用户表格 ----------
        table_box = QGroupBox("用户管理 (0-49)")
        tlay = QVBoxLayout(table_box)
        bar = QHBoxLayout()
        bar.addWidget(QLabel("指纹默认处于验证状态，执行录入/删除等操作会自动切换并恢复。"))
        bar.addStretch(1)
        self.refresh_table_btn = QPushButton("全部刷新")
        self.refresh_table_btn.setEnabled(False)
        bar.addWidget(self.refresh_table_btn)
        tlay.addLayout(bar)

        self.table = QTableWidget(MAX_USERS, 6)
        self.table.setHorizontalHeaderLabels(
            ["ID", "用户名", "通道", "用户数据(密码)", "指纹", "操作"])
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.setSelectionMode(QAbstractItemView.NoSelection)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.Stretch)
        header.setSectionResizeMode(2, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.Stretch)
        header.setSectionResizeMode(4, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(5, QHeaderView.Fixed)
        self.table.setColumnWidth(5, 240)
        self.table.verticalHeader().setDefaultSectionSize(34)
        for row in range(MAX_USERS):
            self._build_row(row)
        tlay.addWidget(self.table)
        root.addWidget(table_box, 1)

        # ---------- 命令行控制台 ----------
        cli_box = QGroupBox("命令行控制台")
        cli_lay = QVBoxLayout(cli_box)
        input_lay = QHBoxLayout()
        self.cmd_input = QLineEdit()
        self.cmd_input.setPlaceholderText("输入 CLI 命令，如 user count / fp_status / bond clear")
        self.cmd_send_btn = QPushButton("发送")
        input_lay.addWidget(self.cmd_input, 1)
        input_lay.addWidget(self.cmd_send_btn)
        cli_lay.addLayout(input_lay)
        self.console = QPlainTextEdit()
        self.console.setReadOnly(True)
        self.console.setMaximumBlockCount(5000)
        cli_lay.addWidget(self.console)
        root.addWidget(cli_box, 0)

        self.setCentralWidget(central)

    def _build_row(self, row):
        # ID
        item = QTableWidgetItem(str(row))
        item.setTextAlignment(Qt.AlignCenter)
        self.table.setItem(row, 0, item)
        # 用户名
        self.table.setItem(row, 1, QTableWidgetItem(""))
        # 通道
        item = QTableWidgetItem("")
        item.setTextAlignment(Qt.AlignCenter)
        self.table.setItem(row, 2, item)
        # 密码
        self.table.setCellWidget(row, 3, PasswordCell(""))
        # 指纹
        item = QTableWidgetItem("")
        item.setTextAlignment(Qt.AlignCenter)
        self.table.setItem(row, 4, item)
        # 操作按钮
        w = QWidget()
        lay = QHBoxLayout(w)
        lay.setContentsMargins(4, 2, 4, 2)
        lay.setSpacing(4)
        b_set = QPushButton("设置信息")
        b_enr = QPushButton("录入指纹")
        b_del = QPushButton("删除")
        for b in (b_set, b_enr, b_del):
            b.setFixedHeight(26)
        b_set.clicked.connect(lambda _=False, r=row: self._set_user(r))
        b_enr.clicked.connect(lambda _=False, r=row: self._enroll_fp(r))
        b_del.clicked.connect(lambda _=False, r=row: self._delete_user(r))
        lay.addWidget(b_set)
        lay.addWidget(b_enr)
        lay.addWidget(b_del)
        self.table.setCellWidget(row, 5, w)

    # ==================================================================
    # 信号绑定
    # ==================================================================

    def _bind_signals(self):
        self.refresh_btn.clicked.connect(self.refresh_ports)
        self.connect_btn.clicked.connect(self._toggle_connect)
        self.bond_btn.clicked.connect(self._bond_clear)
        self.refresh_table_btn.clicked.connect(self._init_all)
        self.cmd_send_btn.clicked.connect(self._send_manual_cmd)
        self.cmd_input.returnPressed.connect(self._send_manual_cmd)

        self._worker.connected.connect(self._on_connected)
        self._worker.disconnected.connect(self._on_disconnected)
        self._worker.error_occurred.connect(self._on_error)
        self._api.line_received.connect(self._on_line)
        self._api.echo_received.connect(self._on_echo)
        self._fp.log.connect(self._on_fp_log)
        self._fp.step.connect(self._on_fp_step)

    # ==================================================================
    # 连接管理
    # ==================================================================

    def refresh_ports(self):
        cur = self.port_combo.currentText()
        self.port_combo.clear()
        for device, desc in list_serial_ports():
            self.port_combo.addItem(desc, device)
        if self.port_combo.count() > 0:
            idx = self.port_combo.findText(cur) if cur else -1
            self.port_combo.setCurrentIndex(idx if idx >= 0 else 0)

    def _toggle_connect(self):
        if self._connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        if self._busy_ui:
            QMessageBox.warning(self, "提示", "当前有任务进行中，请稍候")
            return
        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "提示", "未选择串口，请先刷新并选择串口")
            return
        baud = self.baud_combo.currentText()
        try:
            self._worker.open_port(port, baud)
        except Exception as e:
            QMessageBox.critical(self, "连接失败", str(e))
            return

    def _disconnect(self):
        self._worker.close_port()

    def _on_connected(self, port):
        self._connected = True
        self.connect_btn.setText("断开")
        self.bond_btn.setEnabled(True)
        self.refresh_table_btn.setEnabled(True)
        self._append_console(f">>> 已连接 {port} @ {self.baud_combo.currentText()}")
        self.statusBar().showMessage(f"已连接 {port}，正在初始化用户数据...")
        # 延迟到事件循环，避免阻塞 connected 信号
        QTimer.singleShot(50, self._init_all)

    def _on_disconnected(self):
        was = self._connected
        self._connected = False
        self.connect_btn.setText("连接")
        self.bond_btn.setEnabled(False)
        self.refresh_table_btn.setEnabled(False)
        self.statusBar().showMessage("未连接")
        if was:
            self._append_console(">>> 已断开连接")

    def _on_error(self, msg):
        self._append_console(f"!! 错误: {msg}")

    # ==================================================================
    # 初始化 / 刷新
    # ==================================================================

    def _set_busy(self, busy):
        self._busy_ui = busy
        for row in range(MAX_USERS):
            w = self.table.cellWidget(row, 5)
            if w is not None:
                for b in w.findChildren(QPushButton):
                    b.setEnabled(not busy)

    def _init_all(self):
        if not self._connected or self._busy_ui:
            return
        self._set_busy(True)
        try:
            # 1) user list 一次性读取全部用户
            users = self._api.read_user_list()
            if users is None:
                self._append_console("[用户] user list 无响应或格式异常")
            else:
                for uid, info in users.items():
                    self._users[uid] = info
            if not self._connected:
                return
            for i in range(MAX_USERS):
                self._update_row(i)
            # 2) 读取指纹索引表
            ok, ids, msg = self._fp.read_index()
            self._append_console(f"[指纹] {msg}")
            if ok:
                self._fp_ids = ids
            else:
                self._fp_ids = set()
            for i in range(MAX_USERS):
                self._update_fp_cell(i)
            self.statusBar().showMessage(
                f"初始化完成，已读取 {sum(1 for u in self._users if not u.empty)} 个用户")
        finally:
            self._set_busy(False)

    # ==================================================================
    # 表格更新
    # ==================================================================

    def _update_row(self, row):
        info = self._users[row]
        self.table.item(row, 1).setText(info.name)
        self.table.item(row, 2).setText(info.channel if info.channel else "none")
        cell = self.table.cellWidget(row, 3)
        if cell is not None:
            cell.set_text(info.data)
            cell.reset_mask()  # 重置为掩码
        self._update_fp_cell(row)

    def _update_fp_cell(self, row):
        item = self.table.item(row, 4)
        if item is None:
            return
        if row in self._fp_ids:
            item.setText("已录入")
            item.setForeground(Qt.darkGreen)
        else:
            item.setText("未录入")
            item.setForeground(Qt.gray)

    # ==================================================================
    # 用户操作
    # ==================================================================

    def _set_user(self, uid):
        if not self._connected:
            QMessageBox.warning(self, "提示", "请先连接设备")
            return
        if self._busy_ui:
            QMessageBox.warning(self, "提示", "当前有任务进行中，请稍候")
            return
        cur = self._users[uid]
        dlg = UserDialog(uid, cur.name, cur.channel or "both", cur.data, self)
        if dlg.exec_() != QDialog.Accepted:
            return
        name, ch, data = dlg.values()
        if not name:
            QMessageBox.warning(self, "提示", "用户名不能为空")
            return
        if " " in data:
            QMessageBox.warning(self, "提示", "用户数据(密码)不能包含空格")
            return
        self._set_busy(True)
        try:
            cmd = f"user set {uid} {name} {ch} {data}"
            self._append_console(f">>> {cmd}")
            line = self._api.send_and_wait(
                cmd,
                lambda l: ("user set ok" in l) or ("FAILED" in l) or ("invalid" in l),
                timeout_ms=5000)
            if line and "user set ok" in line:
                self._users[uid] = UserInfo(uid, name, ch, data)
                self._users[uid].has_fingerprint = uid in self._fp_ids
                self._update_row(uid)
                self.statusBar().showMessage(f"ID {uid} 用户信息已保存")
            else:
                QMessageBox.warning(self, "设置失败",
                                    f"ID {uid} 用户信息保存失败:\n{line or '超时'}")
        finally:
            self._set_busy(False)

    def _enroll_fp(self, uid):
        if not self._connected:
            QMessageBox.warning(self, "提示", "请先连接设备")
            return
        if self._busy_ui:
            QMessageBox.warning(self, "提示", "当前有任务进行中，请稍候")
            return
        ret = QMessageBox.question(
            self, "录入指纹",
            f"将为 ID {uid} 录入指纹（共需按压手指 3 次）。\n"
            "点击确定后，请按状态栏提示依次按压、移开手指。\n\n是否开始？")
        if ret != QMessageBox.Yes:
            return
        self._set_busy(True)
        try:
            ok, msg = self._fp.enroll(uid)
            self._append_console(f"[录入] {msg}")
            if ok:
                self._fp_ids.add(uid)
                self._update_fp_cell(uid)
                self.statusBar().showMessage(f"ID {uid} 指纹录入成功")
            else:
                self.statusBar().showMessage(msg)
        finally:
            self._set_busy(False)

    def _delete_user(self, uid):
        if not self._connected:
            QMessageBox.warning(self, "提示", "请先连接设备")
            return
        if self._busy_ui:
            QMessageBox.warning(self, "提示", "当前有任务进行中，请稍候")
            return
        ret = QMessageBox.question(
            self, "删除确认",
            f"确认删除 ID {uid} 的指纹和用户信息？\n（将同时执行 fp_delete 与 user del，不可恢复）",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
        if ret != QMessageBox.Yes:
            return
        self._set_busy(True)
        try:
            # 1) 删除指纹
            ok_fp, msg_fp = self._fp.delete_fp(uid)
            self._append_console(f"[指纹] {msg_fp}")
            # 2) 删除用户信息
            line = self._api.send_and_wait(
                f"user del {uid}",
                lambda l, u=uid: (f"user del ok, id={u}" in l) or
                                 (f"user del FAILED (id={u})" in l),
                timeout_ms=5000)
            self._append_console(f"[用户] {line or 'user del 超时'}")
            if ok_fp and line and f"user del ok, id={uid}" in line:
                self._users[uid] = UserInfo(uid)
                if uid in self._fp_ids:
                    self._fp_ids.remove(uid)
                self._update_row(uid)
                self.statusBar().showMessage(f"ID {uid} 已删除（指纹 + 用户信息）")
            elif ok_fp:
                QMessageBox.warning(self, "删除不完整",
                                    f"指纹已删除，但用户信息删除未成功:\n{line}")
        finally:
            self._set_busy(False)

    # ==================================================================
    # BLE 配对
    # ==================================================================

    def _bond_clear(self):
        if not self._connected:
            return
        ret = QMessageBox.question(
            self, "解除 BLE 配对",
            "将发送 bond clear：解除所有 BLE 配对、断开连接并重新进入可配对广播。\n确定继续？",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
        if ret != QMessageBox.Yes:
            return
        self._api.send("bond clear")
        self._append_console(">>> bond clear")

    # ==================================================================
    # 命令行控制台
    # ==================================================================

    def _send_manual_cmd(self):
        if not self._connected:
            QMessageBox.warning(self, "提示", "请先连接设备")
            return
        cmd = self.cmd_input.text().strip()
        if not cmd:
            return
        self.cmd_input.clear()
        self._api.send(cmd)

    def _append_console(self, text):
        self.console.appendPlainText(text)

    def _on_line(self, line):
        self._append_console(mask_passwords(line))

    def _on_echo(self, line):
        self._append_console(f"> {mask_passwords(line)}")

    def _on_fp_log(self, text):
        self._append_console(text)

    def _on_fp_step(self, msg):
        self.statusBar().showMessage(f"[指纹] {msg}，请按提示操作")

    # ==================================================================
    # 关闭
    # ==================================================================

    def closeEvent(self, event):
        self._worker.close_port()
        super().closeEvent(event)
