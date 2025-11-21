# 调试拍照功能指南

## 已完成的修改

### 后端改进
1. 在 `jpg_httpd_handler` 添加了明显的日志标记（带分隔线）
2. 添加重试机制（最多8次，每次延迟100ms）
3. 打印内存状态
4. 失败时返回友好的 HTML 错误页面

### 前端改进
1. 使用 `addEventListener` 而非内联 onclick
2. 添加详细的 console.log 日志
3. 检查 blob 大小
4. 显示详细的状态信息

## 编译和刷写

```powershell
cd "d:\esp32\camera\ESP32_CAM_WEB_PAGE-main\ESP32_CAM_WEB_PAGE-main"
D:\Espressif\frameworks\esp-idf-v5.3.1\export.ps1
idf.py build
idf.py -p COM3 flash monitor
```

## 测试步骤

### 1. 检查串口启动日志
刷写后，在 monitor 中确认：
- WiFi 连接成功并获得 IP
- 看到 `Registered /capture handler`
- 看到 `Registered /stream handler`

### 2. 测试主页访问
在浏览器打开 `http://<ESP32-IP>/`
- 确认能看到视频流
- 按 F12 打开开发者工具，切换到 Console 标签页

### 3. 测试拍照功能
点击 "Capture Still Photo" 按钮

**预期在浏览器控制台看到：**
```
Starting capture request...
Response received: 200 OK
Blob received, size: XXXXX
```

**预期在串口 monitor 看到：**
```
========================================
[CAPTURE_HANDLER_V2] *** CAPTURE REQUEST RECEIVED ***
[CAPTURE_HANDLER_V2] Capture request received (chunk=1024, retries=5)
[CAPTURE_HANDLER_V2] Free heap: XXXXX bytes
[CAPTURE_HANDLER_V2] Camera capture success (attempt 1)
[CAPTURE_HANDLER_V2] fb->len: XXXXX, fb->format: X, width: 640, height: 480
[CAPTURE_HANDLER_V2] Image sent successfully
```

## 如果没有任何日志输出

### 可能原因1：浏览器缓存了旧页面
**解决方法：**
- 按 Ctrl+Shift+R 强制刷新页面（清除缓存）
- 或打开无痕/隐身窗口重新访问

### 可能原因2：JavaScript 错误
**检查方法：**
1. 浏览器按 F12 打开开发者工具
2. 切换到 Console 标签
3. 查看是否有红色错误信息
4. 将错误信息截图或复制

### 可能原因3：网络问题
**检查方法：**
1. 在浏览器开发者工具切换到 Network 标签
2. 点击拍照按钮
3. 查看是否有 `/capture` 请求
4. 如果有，查看请求状态码和响应

### 可能原因4：HTTP 服务器未正常启动
**检查方法：**
1. 在串口 monitor 中搜索 "Registered /capture handler"
2. 如果没有，说明服务器启动失败
3. 查看之前的错误日志

## 手动测试 capture 端点

在另一个浏览器标签页直接访问：
```
http://<ESP32-IP>/capture
```

- 如果能直接下载或显示图片，说明后端正常，问题在前端
- 如果显示错误页面，查看串口日志
- 如果完全无响应，检查 ESP32 网络连接

## 常见问题排查

### 问题：按钮一直显示 "Capturing..."
**原因：** fetch 请求未完成或失败但未捕获
**排查：**
1. 查看浏览器 Console 是否有错误
2. 查看 Network 标签中 `/capture` 请求的状态
3. 检查串口是否有日志输出

### 问题：显示 "Error: HTTP 503"
**原因：** 摄像头繁忙，所有重试都失败
**解决：**
1. 增加 `max_attempts` 或延迟时间
2. 暂时关闭视频流，单独测试拍照
3. 检查是否 fb_count 太小（当前是4）

### 问题：照片打开但是空白或损坏
**原因：** JPEG 数据传输失败
**排查：**
1. 查看串口中 "Image sent successfully" 日志
2. 检查浏览器 Network 中 `/capture` 响应的大小
3. 查看串口是否有 "Chunk send failed" 错误

## 进一步调试

如果以上步骤都无法解决，请提供：
1. 串口 monitor 的完整输出（从启动到点击拍照）
2. 浏览器控制台的截图或日志
3. 浏览器 Network 标签中 `/capture` 请求的详细信息
