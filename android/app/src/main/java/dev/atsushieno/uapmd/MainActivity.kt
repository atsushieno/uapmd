package dev.atsushieno.uapmd

import android.content.Intent
import android.content.Context
import android.graphics.PixelFormat
import android.os.Build
import android.util.DisplayMetrics
import android.view.View
import android.view.MotionEvent
import android.view.SurfaceControl
import android.view.SurfaceView
import android.view.WindowInsets
import kotlinx.coroutines.runBlocking
import org.androidaudioplugin.hosting.GuiHelper
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface
import dev.atsushieno.uapmd.ui.PluginUiOverlayManager

class MainActivity : SDLActivity() {
    var compositorResumed = false
        private set

    override fun createSDLSurface(context: Context): SDLSurface =
        super.createSDLSurface(context).apply {
            // ImGui clears transparent apertures for the live plugin surfaces below SDL.
            holder.setFormat(PixelFormat.TRANSLUCENT)
            setZOrderOnTop(true)
            if (Build.VERSION.SDK_INT >= 36)
                setCompositionOrder(COMPOSITOR_ORDER)
        }

    fun compositorSurface(): SurfaceView? = mSurface

    fun updateCompositorOrder() {
        val surface = mSurface ?: return
        if (Build.VERSION.SDK_INT >= 36) {
            surface.setCompositionOrder(COMPOSITOR_ORDER)
        } else {
            val control = surface.surfaceControl ?: return
            if (control.isValid)
                SurfaceControl.Transaction().use { it.setLayer(control, COMPOSITOR_ORDER).apply() }
        }
    }

    private fun compositorEvent(event: MotionEvent): MotionEvent {
        val location = IntArray(2)
        mSurface.getLocationOnScreen(location)
        return MotionEvent.obtain(event).apply {
            setLocation(event.rawX - location[0], event.rawY - location[1])
        }
    }

    fun forwardCompositorTouch(event: MotionEvent) {
        val translated = compositorEvent(event)
        try {
            mSurface.onTouch(mSurface, translated)
        } finally {
            translated.recycle()
        }
    }

    fun forwardCompositorMotion(event: MotionEvent): Boolean {
        val translated = compositorEvent(event)
        return try {
            mSurface.dispatchGenericMotionEvent(translated)
        } finally {
            translated.recycle()
        }
    }

    override fun onPause() {
        compositorResumed = false
        PluginUiOverlayManager.closeInputShield()
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        compositorResumed = true
    }

    override fun onDestroy() {
        PluginUiOverlayManager.releaseActivity(this)
        super.onDestroy()
    }

    companion object {
        const val COMPOSITOR_ORDER = 1_000_000
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
            val guiHost = GuiHelper.NativeEmbeddedSurfaceControlHost(
                activity,
                pluginPackageName,
                pluginId,
                instanceId
            )
            return try {
                val preferred = runBlocking { guiHost.getPreferredSizeOrFallback(0, 0) }
                if (preferred.width > 0 && preferred.height > 0)
                    intArrayOf(preferred.width, preferred.height)
                else
                    queryCurrentContentAreaSize(activity)
            } finally {
                guiHost.close()
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
