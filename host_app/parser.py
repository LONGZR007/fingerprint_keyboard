"""固件 CLI 输出解析（纯函数，无第三方依赖，便于单元测试）。

固件输出格式（来自 combo_keyboard/APP 源码）：
  user get 成功:  id=3 name="alice" ch=both data="pass123"
  user get 空槽:  id=3 empty
  [FP] 注册成功, ID=5 / 注册失败, code=0x07
  [FP] 索引表读取成功, 共 128 字节:  后跟 8 行 hex（每行 16 字节 "XX XX ..."）
"""
import re

MAX_USERS = 50
CHANNELS = ("none", "ble", "usb", "both")  # 与 keyboard_channel_name 一致


class UserInfo:
    """一个用户的完整信息。"""

    def __init__(self, uid, name="", channel="none", data=""):
        self.uid = int(uid)
        self.name = name
        self.channel = channel
        self.data = data
        self.has_fingerprint = False

    @property
    def empty(self):
        return not self.name


_RE_USER = re.compile(
    r'id=(\d+)\s+name="([^"]*)"\s+ch=(\w+)\s+data="(.*)"$')
_RE_USER_EMPTY = re.compile(r'id=(\d+)\s+empty$')
_RE_FP = re.compile(r'^\[FP\]\s*(.*)$')
_RE_HEX16 = re.compile(r'^([0-9A-Fa-f]{2} ){15}[0-9A-Fa-f]{2}\s*$')
_RE_DATA = re.compile(r'data="[^"]*"')
_MASK = 'data="***"'
# user list 数据行：掩码 ch 列之后的密码部分
_RE_LIST_MASK = re.compile(
    r"(^\s*\d{1,2} +[^ ]+ +(?:none|ble|usb|both) +).*$")


def parse_user_get(line):
    """解析 'id=3 name="alice" ch=both data="pass"' → UserInfo。
    空槽返回空 UserInfo；无法解析返回 None。"""
    if not line:
        return None
    line = line.strip()
    m = _RE_USER.match(line)
    if m:
        return UserInfo(int(m.group(1)), m.group(2), m.group(3), m.group(4))
    m = _RE_USER_EMPTY.match(line)
    if m:
        return UserInfo(int(m.group(1)))
    return None


# user list 输出行格式：'  %-4u %-16s %-5s %s'
#   表头:   id   name              ch    data
#   数据行: 3    alice             both  pass123
#   结束行: total: 2 user(s)
# name 因 CLI 按空格切分不含空格；ch 为固定枚举。
# 注意：串口层对每行 .strip() 去掉了前导空格，故用 \s* 而非 ^ {2}。
_RE_LIST_ROW = re.compile(
    r"^\s*(\d{1,2}) +([^ ]+) +(none|ble|usb|both) +(\S.*)?$")


def parse_user_list_line(line):
    """解析 'user list' 输出中的一行数据。
    返回 UserInfo；表头行/结束行/无法解析返回 None。"""
    if not line:
        return None
    m = _RE_LIST_ROW.match(line)
    if not m:
        return None
    return UserInfo(int(m.group(1)), m.group(2), m.group(3),
                    (m.group(4) or "").strip())


def parse_fp_line(line):
    """解析 '[FP] xxx' → (消息内容)。非 FP 行返回 None。"""
    if not line:
        return None
    m = _RE_FP.match(line)
    if not m:
        return None
    return m.group(1).strip()


def parse_index_hex(line):
    """解析索引表一行 hex（16 字节）→ [int,...]；非 hex 行返回 None。"""
    if not line:
        return None
    m = _RE_HEX16.match(line)
    if not m:
        return None
    return [int(line[i:i + 2], 16) for i in range(0, 48, 3)]


def index_to_ids(index_bytes):
    """128 字节索引表 → 已录入指纹的 ID 集合。
    ID n 对应 byte[n/8] 的 bit[n%8]。"""
    ids = set()
    for n in range(MAX_USERS):
        byte = index_bytes[n // 8]
        if byte & (1 << (n % 8)):
            ids.add(n)
    return ids


def channel_to_num(ch):
    """通道名 → 数值（none=0 ble=1 usb=2 both=3）"""
    try:
        return CHANNELS.index(ch)
    except ValueError:
        return 0


def mask_passwords(line):
    """把行内 data="..." 或 user list 数据行中的密码替换为 ***，
    防止密码泄露到控制台。"""
    m = _RE_LIST_MASK.match(line)
    if m:
        return m.group(1) + "***"
    return _RE_DATA.sub(_MASK, line)
