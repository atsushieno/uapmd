package dev.atsushieno.uapmd.ui

import android.graphics.Color
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import android.util.TypedValue
import dev.atsushieno.uapmd.MainActivity
import java.util.concurrent.ConcurrentHashMap

private class PluginUiOverlay(
    context: android.content.Context,
    private val handle: Long,
    private val title: String?
) : FrameLayout(context) {
    private val contentContainer = FrameLayout(context)
    private val headerHeightPx = dpToPx(40)
    private val minContentSizePx = dpToPx(200)
    private var contentWidthPx = minContentSizePx
    private var contentHeightPx = minContentSizePx
    private var dragStartX = 0f
    private var dragStartY = 0f
    private var initialLeft = 0
    private var initialTop = 0

    init {
        setBackgroundColor(Color.argb(230, 20, 20, 20))
        elevation = 100f
        clipChildren = true
        clipToPadding = true

        val header = createHeaderView()
        val layout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(header, LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT,
                headerHeightPx))
            addView(contentContainer, LinearLayout.LayoutParams(
                contentWidthPx,
                contentHeightPx))
        }
        addView(layout, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    }

    private fun dpToPx(dp: Int): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp.toFloat(),
            resources.displayMetrics
        ).toInt()
    }

    private fun createHeaderView(): View {
        val headerLayout = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.argb(255, 50, 50, 50))
            setPadding(dpToPx(12), dpToPx(8), dpToPx(12), dpToPx(8))
            gravity = Gravity.CENTER_VERTICAL
        }

        val titleView = TextView(context).apply {
            text = title ?: "Plugin UI"
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            layoutParams = LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
        }

        val closeButton = ImageButton(context).apply {
            setImageResource(android.R.drawable.ic_menu_close_clear_cancel)
            setBackgroundColor(Color.TRANSPARENT)
            setColorFilter(Color.WHITE)
            contentDescription = "Close Plugin UI"
            setOnClickListener {
                PluginUiOverlayManager.setOverlayVisible(handle, false)
                PluginUiOverlayManager.notifyOverlayClosed(handle)
            }
        }

        headerLayout.setOnTouchListener { _, event ->
            val params = layoutParams as? FrameLayout.LayoutParams ?: return@setOnTouchListener false
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    if (params.gravity != Gravity.NO_GRAVITY) {
                        params.leftMargin = left
                        params.topMargin = top
                        params.gravity = Gravity.NO_GRAVITY
                        layoutParams = params
                    }
                    dragStartX = event.rawX
                    dragStartY = event.rawY
                    initialLeft = params.leftMargin
                    initialTop = params.topMargin
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (event.rawX - dragStartX).toInt()
                    val dy = (event.rawY - dragStartY).toInt()
                    params.leftMargin = initialLeft + dx
                    params.topMargin = initialTop + dy
                    params.gravity = Gravity.NO_GRAVITY
                    layoutParams = params
                    true
                }
                MotionEvent.ACTION_UP -> {
                    performClick()
                    true
                }
                else -> false
            }
        }

        headerLayout.addView(titleView)
        headerLayout.addView(closeButton, LinearLayout.LayoutParams(
            LayoutParams.WRAP_CONTENT,
            LayoutParams.WRAP_CONTENT))
        return headerLayout
    }

    val chromeHeight: Int
        get() = headerHeightPx
    val contentWidth: Int
        get() = contentWidthPx
    val contentHeight: Int
        get() = contentHeightPx

    fun updateContentSize(width: Int, height: Int) {
        contentWidthPx = width.coerceAtLeast(minContentSizePx)
        contentHeightPx = height.coerceAtLeast(minContentSizePx)
        val params = contentContainer.layoutParams
        if (params is LinearLayout.LayoutParams) {
            params.width = contentWidthPx
            params.height = contentHeightPx
            contentContainer.layoutParams = params
        } else {
            contentContainer.layoutParams = LinearLayout.LayoutParams(contentWidthPx, contentHeightPx)
        }
        if (childCount > 0) {
            requestLayout()
        }
    }

    fun setSurfaceView(view: View?) {
        contentContainer.removeAllViews()
        if (view != null) {
            val parent = view.parent as? ViewGroup
            parent?.removeView(view)
            contentContainer.addView(view, LayoutParams(contentWidthPx, contentHeightPx))
        }
    }
}

object PluginUiOverlayManager {
    private val handler = Handler(Looper.getMainLooper())
    private val overlays = ConcurrentHashMap<Long, PluginUiOverlay>()
    private const val MIN_DIMENSION_DP = 200

    @JvmStatic
    fun createOverlay(handle: Long, title: String?, width: Int, height: Int) {
        handler.post {
            val activity = MainActivity.getInstance() ?: return@post
            if (overlays.containsKey(handle))
                return@post
            val overlay = PluginUiOverlay(activity, handle, title).apply {
                contentDescription = title ?: "Plugin UI"
                visibility = View.GONE
            }
            overlay.updateContentSize(width, height)
            overlays[handle] = overlay
            val params = FrameLayout.LayoutParams(
                overlay.contentWidth,
                overlay.contentHeight + overlay.chromeHeight
            )
            params.gravity = Gravity.CENTER
            activity.addContentView(overlay, params)
        }
    }

    @JvmStatic
    fun destroyOverlay(handle: Long) {
        handler.post {
            val overlay = overlays.remove(handle) ?: return@post
            val parent = overlay.parent as? ViewGroup
            parent?.removeView(overlay)
        }
    }

    @JvmStatic
    fun setOverlayVisible(handle: Long, visible: Boolean) {
        handler.post {
            overlays[handle]?.visibility = if (visible) View.VISIBLE else View.GONE
        }
    }

    @JvmStatic
    fun resizeOverlay(handle: Long, width: Int, height: Int) {
        handler.post {
            overlays[handle]?.let { overlay ->
                overlay.updateContentSize(width, height)
                val params = overlay.layoutParams as? FrameLayout.LayoutParams
                if (params != null) {
                    params.width = overlay.contentWidth
                    params.height = overlay.contentHeight + overlay.chromeHeight
                    overlay.requestLayout()
                }
            }
        }
    }

    @JvmStatic
    fun attachSurfaceView(handle: Long, surfaceView: View?) {
        handler.post {
            overlays[handle]?.setSurfaceView(surfaceView)
        }
    }

    @JvmStatic
    fun notifyOverlayClosed(handle: Long) {
        MainActivity.nativeOnOverlayClosed(handle)
    }
}
