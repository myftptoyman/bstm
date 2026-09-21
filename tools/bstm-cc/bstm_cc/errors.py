"""bstm-cc 的錯誤型別。所有「不支援」的情況都必須走這裡，不可靜默產生錯誤程式碼。"""


class BstmError(Exception):
    """bstm-cc 的使用者層級錯誤（不是內部 bug）。"""


class UnsupportedCell(BstmError):
    """遇到 dispatch table 沒有的 cell 型別，或該型別的某個組態不支援。"""

    def __init__(self, cell_name, cell_type, src=None, detail=None):
        self.cell_name = cell_name
        self.cell_type = cell_type
        self.src = src
        self.detail = detail
        loc = src if src else "<未知來源>"
        msg = "不支援的 cell: %s  (type=%s)\n            來源: %s" % (
            cell_name, cell_type, loc)
        if detail:
            msg += "\n            原因: " + detail
        super().__init__(msg)


class CombLoop(BstmError):
    """組合迴圈：拓樸排序無法完成。"""


def src_of(cell):
    a = cell.get("attributes") or {}
    return a.get("src")
