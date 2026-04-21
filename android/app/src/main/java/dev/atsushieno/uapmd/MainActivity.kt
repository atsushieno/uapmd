package dev.atsushieno.uapmd

import android.content.Intent
import android.os.Build
import android.util.DisplayMetrics
import android.view.View
import android.view.WindowInsets
import org.androidaudioplugin.hosting.AudioPluginHostHelper
import org.libsdl.app.SDLActivity

class MainActivity : SDLActivity() {
    companion object {
        @JvmStatic external fun nativeExecuteUiThreadTask(token: Long)
        @JvmStatic external fun nativeHandleActivityResult(requestCode: Int, resultCode: Int, data: Intent?)
        @JvmStatic external fun nativeOnOverlayClosed(handle: Long)
        @JvmStatic external fun nativeOnOverlaySurfaceReady(handle: Long)
        @JvmStatic external fun nativeConfigureOverlayViewport(
            handle: Long,
            viewportWidth: Int,
            viewportHeight: Int,
            contentWidth: Int,
            contentHeight: Int,
            scrollX: Int,
            scrollY: Int
        )
        @JvmStatic external fun nativeRunAutomationScript(code: String): String
        @JvmStatic external fun nativeStartAutomationScriptJob(code: String): String
        @JvmStatic external fun nativeGetAutomationScriptJob(jobId: String): String
        @JvmStatic external fun nativeClearAutomationScriptJob(jobId: String)

        @JvmStatic
        fun getInstance(): SDLActivity? = SDLActivity.mSingleton

        @JvmStatic
        fun postNativeUiTask(token: Long) {
            val activity = SDLActivity.mSingleton
            if (activity != null) {
                activity.runOnUiThread { nativeExecuteUiThreadTask(token) }
            } else {
                nativeExecuteUiThreadTask(token)
            }
        }

        @JvmStatic
        fun queryRemoteViewPreferredSize(
            pluginPackageName: String,
            pluginId: String,
            instanceId: Int
        ): IntArray {
            val activity = SDLActivity.mSingleton ?: return intArrayOf(0, 0)
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
                return queryCurrentContentAreaSize(activity)
            val surfaceControl = AudioPluginHostHelper.createSurfaceControl(activity)
            return try {
                val preferred = surfaceControl.getPreferredSize(pluginPackageName, pluginId, instanceId)
                if (preferred.getOrNull(0)?.let { it > 0 } == true &&
                    preferred.getOrNull(1)?.let { it > 0 } == true)
                    preferred
                else
                    queryCurrentContentAreaSize(activity)
            } finally {
                surfaceControl.close()
            }
        }

        private fun queryCurrentContentAreaSize(activity: SDLActivity): IntArray {
            val contentRoot = activity.findViewById<View>(android.R.id.content)
            val rootWidth = contentRoot?.width ?: 0
            val rootHeight = contentRoot?.height ?: 0
            if (rootWidth > 0 && rootHeight > 0)
                return intArrayOf(rootWidth, rootHeight)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val metrics = activity.windowManager.currentWindowMetrics
                val systemBars = metrics.windowInsets.getInsets(WindowInsets.Type.systemBars())
                return intArrayOf(
                    metrics.bounds.width() - systemBars.left - systemBars.right,
                    metrics.bounds.height() - systemBars.top - systemBars.bottom
                )
            }

            val metrics = DisplayMetrics()
            @Suppress("DEPRECATION")
            activity.windowManager.defaultDisplay.getMetrics(metrics)
            var availableWidth = metrics.widthPixels
            var availableHeight = metrics.heightPixels
            @Suppress("DEPRECATION")
            activity.window.decorView.rootWindowInsets?.let { insets ->
                availableWidth -= insets.systemWindowInsetLeft + insets.systemWindowInsetRight
                availableHeight -= insets.systemWindowInsetTop + insets.systemWindowInsetBottom
            }
            return intArrayOf(availableWidth, availableHeight)
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        nativeHandleActivityResult(requestCode, resultCode, data)
    }
}
