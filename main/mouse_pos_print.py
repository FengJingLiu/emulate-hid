import pyautogui
import time

try:
    while True:
        # 获取鼠标的当前位置
        x, y = pyautogui.position()
        print(f"Mouse position: ({x}, {y})")
        # 暂停一段时间，以免过于频繁地刷新位置
        time.sleep(0.1)
except KeyboardInterrupt:
    print("Program terminated.")
