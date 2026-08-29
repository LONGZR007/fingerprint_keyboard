"""指纹键盘 PyQt5 上位机入口。

用法：
    python main.py
"""
import sys

from PyQt5.QtWidgets import QApplication

from main_window import MainWindow


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("指纹键盘上位机")
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
