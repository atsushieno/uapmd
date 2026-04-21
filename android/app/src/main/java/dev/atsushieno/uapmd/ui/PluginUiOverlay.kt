package dev.atsushieno.uapmd.ui

import android.app.Activity
import android.graphics.Color
import android.os.Handler
import android.os.Looper
import android.util.DisplayMetrics
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import dev.atsushieno.uapmd.MainActivity
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.roundToInt

private class OverlayScrollBar(
    context: android.content.Context,
    private val vertical: Boolean,
    private val thicknessPx: Int,
    private val thumbLengthPx: Int,
    private val onScrollRequested: (Int) -> Unit
) : FrameLayout(context) {
    private val trackView = View(context)
    private val thumbView = View(context)
    private var range = 0
    private var position = 0

    init {
        clipChildren = false
        clipToPadding = false
        setBackgroundColor(Color.TRANSPARENT)

        trackView.setBackgroundColor(Color.argb(48, 255, 255, 255))
        thumbView.setBackgroundColor(Color.argb(216, 255, 255, 255))

        addView(trackView)
        addView(thumbView)

        visibility = View.INVISIBLE
        isClickable = true
        isFocusable = false

        setOnTouchListener { _, event ->
            if (range <= 0)
                return@setOnTouchListener false
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                    val extent = if (vertical) height else width
                    val thumbTravel = (extent - thumbLengthPx).coerceAtLeast(1)
                    val coordinate = if (vertical) event.y else event.x
                    val desired = (coordinate - thumbLengthPx / 2f).coerceIn(0f, thumbTravel.toFloat())
                    val newPosition = ((desired / thumbTravel) * range).roundToInt().coerceIn(0, range)
                    onScrollRequested(newPosition)
                    true
                }
                else -> false
            }
        }
    }

    fun updateMetrics(range: Int, position: Int) {
        this.range = range.coerceAtLeast(0)
        this.position = position.coerceIn(0, this.range)
        visibility = if (this.range > 0) View.VISIBLE else View.INVISIBLE
        requestLayout()
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        val extent = if (vertical) height else width
        val cross = if (vertical) width else height
        val trackInset = ((cross - thicknessPx) / 2).coerceAtLeast(0)

        if (vertical) {
            trackView.layout(trackInset, 0, trackInset + thicknessPx, height)
        } else {
            trackView.layout(0, trackInset, width, trackInset + thicknessPx)
        }

        val thumbTravel = (extent - thumbLengthPx).coerceAtLeast(0)
        val thumbOffset = if (range > 0) ((position.toFloat() / range.toFloat()) * thumbTravel).roundToInt() else 0
        if (vertical) {
            thumbView.layout(0, thumbOffset, width, thumbOffset + thumbLengthPx)
        } else {
            thumbView.layout(thumbOffset, 0, thumbOffset + thumbLengthPx, height)
        }
    }
}

private class PluginUiOverlay(
    context: android.content.Context,
    private val handle: Long,
    private val title: String?
) : FrameLayout(context) {
    private val contentContainer = FrameLayout(context)
    private val headerHeightPx = dpToPx(40)
    private val minContentSizePx = dpToPx(200)
    private val scrollBarThicknessPx = dpToPx(14)
    private val scrollBarThumbLengthPx = dpToPx(56)
    private val verticalScrollBar = OverlayScrollBar(
        context,
        vertical = true,
        thicknessPx = dpToPx(6),
        thumbLengthPx = scrollBarThumbLengthPx
    ) { updateScroll(scroll_x = scrollXPos, scroll_y = it, notifyNative = true) }
    private val horizontalScrollBar = OverlayScrollBar(
        context,
        vertical = false,
        thicknessPx = dpToPx(6),
        thumbLengthPx = scrollBarThumbLengthPx
    ) { updateScroll(scroll_x = it, scroll_y = scrollYPos, notifyNative = true) }

    private var contentWidthPx = minContentSizePx
    private var contentHeightPx = minContentSizePx
    private var viewportWidthPx = minContentSizePx
    private var viewportHeightPx = minContentSizePx
    private var scrollXPos = 0
    private var scrollYPos = 0
    private var dragStartX = 0f
    private var dragStartY = 0f
    private var initialLeft = 0
    private var initialTop = 0
    private var surfaceReadyNotified = false

    init {
        setBackgroundColor(Color.argb(230, 20, 20, 20))
        elevation = 100f
        clipChildren = true
        clipToPadding = true

        val header = createHeaderView()
        val viewportRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(contentContainer, LinearLayout.LayoutParams(viewportWidthPx, viewportHeightPx))
            addView(verticalScrollBar, LinearLayout.LayoutParams(scrollBarThicknessPx, viewportHeightPx))
        }
        val bottomRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(horizontalScrollBar, LinearLayout.LayoutParams(viewportWidthPx, scrollBarThicknessPx))
            addView(View(context), LinearLayout.LayoutParams(scrollBarThicknessPx, scrollBarThicknessPx))
        }
        val layout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(header, LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, headerHeightPx))
            addView(viewportRow, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
            addView(bottomRow, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        }
        addView(layout, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    }

    private fun dpToPx(dp: Int): Int =
        TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp.toFloat(),
            resources.displayMetrics
        ).toInt()

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
        headerLayout.addView(closeButton, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        return headerLayout
    }

    val chromeHeight: Int
        get() = headerHeightPx
    val totalWidth: Int
        get() = viewportWidthPx + scrollBarThicknessPx
    val totalHeight: Int
        get() = headerHeightPx + viewportHeightPx + scrollBarThicknessPx

    fun updateContentSize(width: Int, height: Int, notifyNative: Boolean) {
        contentWidthPx = width.coerceAtLeast(minContentSizePx)
        contentHeightPx = height.coerceAtLeast(minContentSizePx)
        clampScroll()
        syncBars()
        updateAttachedViewSize()
        if (notifyNative)
            notifyViewportConfiguration()
    }

    fun updateViewportSize(width: Int, height: Int, notifyNative: Boolean) {
        viewportWidthPx = width.coerceAtLeast(minContentSizePx)
        viewportHeightPx = height.coerceAtLeast(minContentSizePx)

        (contentContainer.layoutParams as? LinearLayout.LayoutParams)?.let {
            it.width = viewportWidthPx
            it.height = viewportHeightPx
            contentContainer.layoutParams = it
        }
        (verticalScrollBar.layoutParams as? LinearLayout.LayoutParams)?.let {
            it.width = scrollBarThicknessPx
            it.height = viewportHeightPx
            verticalScrollBar.layoutParams = it
        }
        (horizontalScrollBar.layoutParams as? LinearLayout.LayoutParams)?.let {
            it.width = viewportWidthPx
            it.height = scrollBarThicknessPx
            horizontalScrollBar.layoutParams = it
        }

        updateAttachedViewSize()
        clampScroll()
        syncBars()
        if (notifyNative)
            notifyViewportConfiguration()
    }

    fun setSurfaceView(view: View?) {
        surfaceReadyNotified = false
        contentContainer.removeAllViews()
        if (view != null) {
            val parent = view.parent as? ViewGroup
            parent?.removeView(view)
            contentContainer.addView(view, LayoutParams(viewportWidthPx, viewportHeightPx))
            scheduleSurfaceReadyNotification(view)
        }
    }

    private fun scheduleSurfaceReadyNotification(view: View) {
        // Ordering matters for remote plugin UI setup:
        // 1. attach SurfaceView to a live host view tree,
        // 2. wait until Android gives it real attachment/display/layout state,
        // 3. connect the remote UI,
        // 4. only then start viewport-driven scrolling updates.
        // Triggering viewport/configuration work before this point races the
        // SurfaceControlViewHost setup and causes compatibility issues.
        val notifyWhenReady = object : Runnable {
            override fun run() {
                if (surfaceReadyNotified)
                    return
                if (!view.isAttachedToWindow || view.display == null || view.layoutParams == null) {
                    view.post(this)
                    return
                }
                surfaceReadyNotified = true
                MainActivity.nativeOnOverlaySurfaceReady(handle)
                notifyViewportConfiguration()
            }
        }
        view.post(notifyWhenReady)
    }

    private fun updateAttachedViewSize() {
        if (contentContainer.childCount > 0)
            contentContainer.getChildAt(0).layoutParams = LayoutParams(viewportWidthPx, viewportHeightPx)
    }

    private fun clampScroll() {
        scrollXPos = scrollXPos.coerceIn(0, (contentWidthPx - viewportWidthPx).coerceAtLeast(0))
        scrollYPos = scrollYPos.coerceIn(0, (contentHeightPx - viewportHeightPx).coerceAtLeast(0))
    }

    private fun syncBars() {
        verticalScrollBar.updateMetrics((contentHeightPx - viewportHeightPx).coerceAtLeast(0), scrollYPos)
        horizontalScrollBar.updateMetrics((contentWidthPx - viewportWidthPx).coerceAtLeast(0), scrollXPos)
    }

    private fun updateScroll(scroll_x: Int, scroll_y: Int, notifyNative: Boolean) {
        scrollXPos = scroll_x
        scrollYPos = scroll_y
        clampScroll()
        syncBars()
        if (notifyNative)
            notifyViewportConfiguration()
    }

    private fun notifyViewportConfiguration() {
        MainActivity.nativeConfigureOverlayViewport(
            handle,
            viewportWidthPx,
            viewportHeightPx,
            contentWidthPx,
            contentHeightPx,
            scrollXPos,
            scrollYPos
        )
    }
}

object PluginUiOverlayManager {
    private val handler = Handler(Looper.getMainLooper())
    private val overlays = ConcurrentHashMap<Long, PluginUiOverlay>()
    private const val MIN_DIMENSION_DP = 200
    private const val HEADER_HEIGHT_DP = 40
    private const val MAX_HOST_FRACTION = 0.8f
    private const val SCROLLBAR_THICKNESS_DP = 14

    private fun dpToPx(activity: Activity, dp: Int): Int =
        TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp.toFloat(),
            activity.resources.displayMetrics
        ).toInt()

    private fun constrainContentSize(activity: Activity, width: Int, height: Int): IntArray {
        val minDimensionPx = dpToPx(activity, MIN_DIMENSION_DP)
        val headerHeightPx = dpToPx(activity, HEADER_HEIGHT_DP)
        val scrollbarPx = dpToPx(activity, SCROLLBAR_THICKNESS_DP)
        val contentRoot = activity.findViewById<View>(android.R.id.content)
        val rootWidth = contentRoot?.width ?: 0
        val rootHeight = contentRoot?.height ?: 0
        if (rootWidth > 0 && rootHeight > 0) {
            return constrainContentSize(width, height, rootWidth, rootHeight, minDimensionPx, headerHeightPx, scrollbarPx)
        }

        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            val metrics = activity.windowManager.currentWindowMetrics
            val systemBars = metrics.windowInsets.getInsets(WindowInsets.Type.systemBars())
            return constrainContentSize(
                width,
                height,
                metrics.bounds.width() - systemBars.left - systemBars.right,
                metrics.bounds.height() - systemBars.top - systemBars.bottom,
                minDimensionPx,
                headerHeightPx,
                scrollbarPx
            )
        } else {
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
            return constrainContentSize(
                width,
                height,
                availableWidth,
                availableHeight,
                minDimensionPx,
                headerHeightPx,
                scrollbarPx
            )
        }
    }

    private fun constrainContentSize(
        width: Int,
        height: Int,
        availableWidth: Int,
        availableHeight: Int,
        minContentSizePx: Int,
        headerHeightPx: Int,
        scrollbarThicknessPx: Int
    ): IntArray {
        val maxContentWidth = ((availableWidth - scrollbarThicknessPx) * MAX_HOST_FRACTION).toInt().coerceAtLeast(1)
        val maxContentHeight = (((availableHeight - headerHeightPx - scrollbarThicknessPx) * MAX_HOST_FRACTION)).toInt().coerceAtLeast(1)
        val minContentWidth = minContentSizePx.coerceAtMost(maxContentWidth)
        val minContentHeight = minContentSizePx.coerceAtMost(maxContentHeight)
        return intArrayOf(
            width.coerceIn(minContentWidth, maxContentWidth),
            height.coerceIn(minContentHeight, maxContentHeight)
        )
    }

    @JvmStatic
    fun constrainContentSize(width: Int, height: Int): IntArray {
        val activity = MainActivity.getInstance() ?: return intArrayOf(width, height)
        return constrainContentSize(activity, width, height)
    }

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
            val constrained = constrainContentSize(activity, width, height)
            overlay.updateContentSize(width, height, notifyNative = false)
            overlay.updateViewportSize(constrained[0], constrained[1], notifyNative = false)
            overlays[handle] = overlay
            val params = FrameLayout.LayoutParams(overlay.totalWidth, overlay.totalHeight)
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
            val activity = MainActivity.getInstance() ?: return@post
            overlays[handle]?.let { overlay ->
                val constrained = constrainContentSize(activity, width, height)
                overlay.updateContentSize(width, height, notifyNative = false)
                overlay.updateViewportSize(constrained[0], constrained[1], notifyNative = true)
                val params = overlay.layoutParams as? FrameLayout.LayoutParams
                if (params != null) {
                    params.width = overlay.totalWidth
                    params.height = overlay.totalHeight
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
