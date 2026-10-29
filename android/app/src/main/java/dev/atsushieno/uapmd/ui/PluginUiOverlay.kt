package dev.atsushieno.uapmd.ui

import android.annotation.SuppressLint
import android.graphics.Color
import android.graphics.Rect
import android.graphics.Region
import android.graphics.RegionIterator
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.SurfaceControl
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.PopupWindow
import androidx.core.graphics.drawable.toDrawable
import dev.atsushieno.uapmd.MainActivity
import org.androidaudioplugin.hosting.AudioPluginSurfaceControlClient
import java.util.concurrent.ConcurrentHashMap

/** Hosts live remote content; ImGui owns the title bar, resizing, scrolling and focus. */
@SuppressLint("ViewConstructor", "RtlHardcoded") // Runtime-only view; coordinates are SDL pixels from the left.
private class PluginUiOverlay(context: android.content.Context, private val handle: Long) : FrameLayout(context) {
    private var surfaceView: SurfaceView? = null
    private var client: AudioPluginSurfaceControlClient? = null
    private var contentWidth = 800
    private var contentHeight = 600
    private var viewportWidth = 800
    private var viewportHeight = 600
    private var requestedViewportWidth = 800
    private var requestedViewportHeight = 600
    private var sizeRequestRevision = 0
    private var scrollXPos = 0
    private var scrollYPos = 0
    private var surfaceReady = false
    private var disposed = false
    private var framed = false
    private var active = false
    private var order = 1
    private var lastConfiguration: List<Int>? = null
    private var lastFrame: List<Int>? = null
    @Volatile var info = intArrayOf(contentWidth, contentHeight, requestedViewportWidth, requestedViewportHeight, sizeRequestRevision)
        private set

    var requestedVisible = false
        set(value) {
            field = value
            visibility = if (value && framed) View.VISIBLE else View.INVISIBLE
        }

    private val contentSizeChanged: (Int, Int) -> Unit = { width, height ->
        post {
            if (!disposed && surfaceReady && width > 0 && height > 0 &&
                (width != contentWidth || height != contentHeight)) {
                contentWidth = width
                contentHeight = height
                // The remote editor already applied this layout. Acknowledge its content
                // size locally instead of echoing it back as another relayout request.
                lastConfiguration = lastConfiguration?.toMutableList()?.apply {
                    this[2] = width
                    this[3] = height
                }
                publishInfo()
            }
        }
    }

    private val surfaceCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) = updateSurfaceOrder()
        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = updateSurfaceOrder()
        override fun surfaceDestroyed(holder: SurfaceHolder) = Unit
    }

    init {
        clipChildren = true
        clipToPadding = true
        addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            updateSurfaceOrder()
            if (surfaceReady)
                notifyViewportConfiguration()
        }
    }

    private fun publishInfo() {
        // Measurements from the plugin and actual frame updates must never become
        // requests to resize the host window. Only resizeContent() advances this revision.
        info = intArrayOf(contentWidth, contentHeight, requestedViewportWidth, requestedViewportHeight, sizeRequestRevision)
    }

    fun resizeContent(width: Int, height: Int) {
        contentWidth = width.coerceAtLeast(1)
        contentHeight = height.coerceAtLeast(1)
        val size = PluginUiOverlayManager.constrainContentSize(contentWidth, contentHeight)
        requestedViewportWidth = size[0]
        requestedViewportHeight = size[1]
        viewportWidth = requestedViewportWidth
        viewportHeight = requestedViewportHeight
        ++sizeRequestRevision
        lastFrame = null
        publishInfo()
    }

    fun attachSurface(view: View?) {
        client?.contentSizeChangedListeners?.remove(contentSizeChanged)
        surfaceView?.holder?.removeCallback(surfaceCallback)
        surfaceReady = false
        lastConfiguration = null
        removeAllViews()
        surfaceView = view as? SurfaceView
        client = null
        if (view == null)
            return
        (view.parent as? ViewGroup)?.removeView(view)
        surfaceView?.setZOrderOnTop(true)
        surfaceView?.holder?.addCallback(surfaceCallback)
        addView(view, LayoutParams(viewportWidth, viewportHeight))
        client = AudioPluginSurfaceControlClient.fromSurfaceView(view)
        client?.contentSizeChangedListeners?.add(contentSizeChanged)
        val notifyWhenReady = object : Runnable {
            override fun run() {
                if (disposed || surfaceView !== view || surfaceReady)
                    return
                if (!view.isAttachedToWindow || view.display == null || view.width <= 0 || view.height <= 0 ||
                    !view.holder.surface.isValid) {
                    view.postOnAnimation(this)
                    return
                }
                surfaceReady = true
                MainActivity.nativeOnOverlaySurfaceReady(handle)
                notifyViewportConfiguration()
                updateSurfaceOrder()
            }
        }
        view.post(notifyWhenReady)
    }

    fun setFrame(x: Int, y: Int, width: Int, height: Int, scrollX: Int, scrollY: Int, rank: Int) {
        framed = true
        visibility = if (requestedVisible) View.VISIBLE else View.INVISIBLE
        order = rank + 1
        val frame = listOf(x, y, width, height, scrollX, scrollY)
        if (lastFrame != frame) {
            lastFrame = frame
            viewportWidth = width.coerceAtLeast(1)
            viewportHeight = height.coerceAtLeast(1)
            scrollXPos = scrollX.coerceIn(0, (contentWidth - viewportWidth).coerceAtLeast(0))
            scrollYPos = scrollY.coerceIn(0, (contentHeight - viewportHeight).coerceAtLeast(0))
            layoutParams = LayoutParams(viewportWidth, viewportHeight).apply {
                gravity = Gravity.TOP or Gravity.LEFT
                leftMargin = x
                topMargin = y
            }
            surfaceView?.layoutParams = LayoutParams(viewportWidth, viewportHeight)
            publishInfo()
        }
        updateSurfaceOrder()
        if (surfaceReady)
            notifyViewportConfiguration()
    }

    fun suspendFrame() {
        framed = false
        updateFocus(false)
        visibility = View.INVISIBLE
    }

    fun updateFocus(focused: Boolean) {
        if (active == focused)
            return
        active = focused
        if (focused)
            surfaceView?.requestFocus()
        else
            surfaceView?.clearFocus()
        updateSurfaceOrder()
    }

    private fun updateSurfaceOrder() {
        val view = surfaceView ?: return
        // Transparent pixels in SDL still count as an obscuring surface to Android's
        // input dispatcher. Only an unobscured, focused editor may be lifted above SDL;
        // covered editors stay in the composited stack and are activated through ImGui.
        val compositionOrder = if (active) MainActivity.COMPOSITOR_ORDER + 1 else order
        if (Build.VERSION.SDK_INT >= 36) {
            view.setCompositionOrder(compositionOrder)
        } else {
            // Earlier releases expose only two SurfaceView orders. Reapply after framework
            // surface/layout updates so their default order cannot replace our shared order.
            val control = view.surfaceControl ?: return
            if (control.isValid)
                SurfaceControl.Transaction().use { it.setLayer(control, compositionOrder).apply() }
        }
    }

    private fun notifyViewportConfiguration() {
        val view = surfaceView ?: return
        if (view.width != viewportWidth || view.height != viewportHeight)
            return
        val configuration = listOf(viewportWidth, viewportHeight, contentWidth, contentHeight, scrollXPos, scrollYPos)
        if (lastConfiguration == configuration)
            return
        lastConfiguration = configuration
        MainActivity.nativeConfigureOverlayViewport(
            handle, viewportWidth, viewportHeight, contentWidth, contentHeight, scrollXPos, scrollYPos)
    }

    fun dispose() {
        disposed = true
        attachSurface(null)
    }
}

/**
 * A transparent input window above the embedded plugin input windows. Its touchable region
 * excludes the visible part of the focused editor, so Android delivers those gestures directly
 * to the plugin. Host controls and background-editor activation go through SDL as usual.
 */
@SuppressLint("ClickableViewAccessibility", "RtlHardcoded") // Forwards input to SDL; positions use screen pixels.
private class HostInputShield(private val activity: MainActivity) {
    private val windows = mutableListOf<PopupWindow>()
    private var lastRegion: Region? = null
    private var lastBounds: Rect? = null

    private fun createWindow(): PopupWindow {
        val content = View(activity).apply {
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
            setOnTouchListener { _, event ->
                activity.forwardCompositorTouch(event)
                true
            }
            setOnGenericMotionListener { _, event -> activity.forwardCompositorMotion(event) }
        }
        return PopupWindow(content, 1, 1, false).apply {
            setBackgroundDrawable(Color.TRANSPARENT.toDrawable())
            isTouchable = true
            isOutsideTouchable = false
            isClippingEnabled = false
            setIsLaidOutInScreen(true)
            inputMethodMode = PopupWindow.INPUT_METHOD_NOT_NEEDED
            setTouchModal(false)
        }
    }

    fun update(region: Region, bounds: Rect) {
        if (lastRegion == region && lastBounds == bounds)
            return
        lastRegion = Region(region)
        lastBounds = Rect(bounds)
        val rectangles = mutableListOf<Rect>()
        if (Build.VERSION.SDK_INT >= 33) {
            if (!region.isEmpty)
                rectangles.add(Rect(0, 0, bounds.width(), bounds.height()))
        } else {
            // API 31/32 do not have AttachedSurfaceControl.setTouchableRegion().
            val iterator = RegionIterator(region)
            val rect = Rect()
            while (iterator.next(rect))
                rectangles.add(Rect(rect))
        }
        while (windows.size > rectangles.size)
            windows.removeAt(windows.lastIndex).dismiss()
        for ((index, rectangle) in rectangles.withIndex()) {
            val window = windows.getOrNull(index) ?: createWindow().also { windows.add(it) }
            if (!window.isShowing) {
                window.width = rectangle.width()
                window.height = rectangle.height()
                window.showAtLocation(activity.window.decorView, Gravity.TOP or Gravity.LEFT,
                    bounds.left + rectangle.left, bounds.top + rectangle.top)
            } else {
                window.update(bounds.left + rectangle.left, bounds.top + rectangle.top, rectangle.width(), rectangle.height())
            }
            if (Build.VERSION.SDK_INT >= 33) {
                window.contentView.post {
                    window.contentView.rootSurfaceControl?.setTouchableRegion(region)
                }
            }
        }
    }

    fun close() {
        windows.forEach { it.dismiss() }
        windows.clear()
        lastRegion = null
        lastBounds = null
    }
}

object PluginUiOverlayManager {
    private val handler = Handler(Looper.getMainLooper())
    private val overlays = ConcurrentHashMap<Long, PluginUiOverlay>()
    private var inputShield: HostInputShield? = null

    @JvmStatic
    fun constrainContentSize(width: Int, height: Int): IntArray {
        val activity = MainActivity.getInstance()
        val root = activity?.findViewById<View>(android.R.id.content)
        val metrics = activity?.resources?.displayMetrics
        val availableWidth = root?.width?.takeIf { it > 0 } ?: metrics?.widthPixels ?: width
        val availableHeight = root?.height?.takeIf { it > 0 } ?: metrics?.heightPixels ?: height
        return intArrayOf(
            width.coerceIn(1, (availableWidth * 0.8f).toInt().coerceAtLeast(1)),
            height.coerceIn(1, (availableHeight * 0.8f).toInt().coerceAtLeast(1)))
    }

    @JvmStatic
    fun createOverlay(handle: Long, title: String?, width: Int, height: Int) {
        handler.post {
            val activity = MainActivity.getInstance() ?: return@post
            if (overlays.containsKey(handle))
                return@post
            val overlay = PluginUiOverlay(activity, handle).apply {
                contentDescription = title ?: "Plugin UI"
                visibility = View.INVISIBLE
                resizeContent(width, height)
            }
            overlays[handle] = overlay
            activity.addContentView(overlay, FrameLayout.LayoutParams(overlay.info[2], overlay.info[3]))
        }
    }

    @JvmStatic
    fun destroyOverlay(handle: Long) {
        handler.post {
            val overlay = overlays.remove(handle) ?: return@post
            overlay.dispose()
            (overlay.parent as? ViewGroup)?.removeView(overlay)
            if (overlays.isEmpty()) {
                inputShield?.close()
                inputShield = null
            }
        }
    }

    @JvmStatic
    fun setOverlayVisible(handle: Long, visible: Boolean) {
        handler.post { overlays[handle]?.requestedVisible = visible }
    }

    @JvmStatic
    fun resizeOverlay(handle: Long, width: Int, height: Int) {
        handler.post { overlays[handle]?.resizeContent(width, height) }
    }

    @JvmStatic
    fun attachSurfaceView(handle: Long, surfaceView: View?) {
        handler.post { overlays[handle]?.attachSurface(surfaceView) }
    }

    @JvmStatic
    fun getOverlayInfo(handle: Long): IntArray? = overlays[handle]?.info

    /** Frames are in SDL surface pixels: x, y, width, height, scrollX, scrollY. */
    @JvmStatic
    fun syncOverlayFrames(handles: LongArray, frames: IntArray, activeHandle: Long, occluders: IntArray) {
        handler.post {
            val activity = MainActivity.getInstance() as? MainActivity ?: return@post
            if (!activity.compositorResumed)
                return@post
            val surface = activity.compositorSurface() ?: return@post
            activity.updateCompositorOrder()
            val surfaceLocation = IntArray(2).also { surface.getLocationOnScreen(it) }
            val root = activity.findViewById<View>(android.R.id.content)
            val rootLocation = IntArray(2).also { root.getLocationOnScreen(it) }
            val passthrough = Region()
            overlays.filterKeys { it !in handles }.values.forEach { it.suspendFrame() }
            for ((index, handle) in handles.withIndex()) {
                val overlay = overlays[handle] ?: continue
                val offset = index * 6
                if (offset + 6 > frames.size)
                    break
                val x = frames[offset]
                val y = frames[offset + 1]
                val width = frames[offset + 2]
                val height = frames[offset + 3]
                overlay.setFrame(x + surfaceLocation[0] - rootLocation[0], y + surfaceLocation[1] - rootLocation[1],
                    width, height, frames[offset + 4], frames[offset + 5], index)
                overlay.updateFocus(handle == activeHandle && overlay.requestedVisible)
                if (handle == activeHandle && overlay.requestedVisible)
                    passthrough.set(x, y, x + width, y + height)
            }
            for (offset in occluders.indices step 4)
                if (offset + 4 <= occluders.size)
                    passthrough.op(occluders[offset], occluders[offset + 1], occluders[offset + 2], occluders[offset + 3], Region.Op.DIFFERENCE)
            val hostRegion = Region(0, 0, surface.width, surface.height)
            hostRegion.op(passthrough, Region.Op.DIFFERENCE)
            if (handles.isEmpty()) {
                inputShield?.close()
                inputShield = null
            } else {
                val shield = inputShield ?: HostInputShield(activity).also { inputShield = it }
                shield.update(hostRegion, Rect(surfaceLocation[0], surfaceLocation[1],
                    surfaceLocation[0] + surface.width, surfaceLocation[1] + surface.height))
            }
        }
    }

    @JvmStatic
    fun closeInputShield() {
        handler.post {
            inputShield?.close()
            inputShield = null
        }
    }

    // Activity lifecycle callbacks run on the UI thread, like all view mutations above.
    fun releaseActivity(activity: MainActivity) {
        for ((handle, overlay) in overlays.entries.toList())
            if (overlay.context === activity) {
                overlays.remove(handle)
                overlay.dispose()
                (overlay.parent as? ViewGroup)?.removeView(overlay)
            }
        inputShield?.close()
        inputShield = null
    }
}
