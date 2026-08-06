#pragma once

namespace uapmd_plugin_hosting::ipc {

inline constexpr char kScannerMsgHello[] = "hello";
inline constexpr char kScannerMsgStartScan[] = "startScan";
inline constexpr char kScannerMsgBundleStarted[] = "bundleStarted";
inline constexpr char kScannerMsgBundleFinished[] = "bundleFinished";
inline constexpr char kScannerMsgBundleTotals[] = "bundleTotals";
inline constexpr char kScannerMsgScanResult[] = "scanResult";
inline constexpr char kScannerMsgCancelScan[] = "cancelScan";

inline constexpr int kScannerProtocolVersion = 1;

} // namespace uapmd_plugin_hosting::ipc
