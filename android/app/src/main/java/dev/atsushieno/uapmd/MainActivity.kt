package dev.atsushieno.uapmd

import android.content.Intent
import org.libsdl.app.SDLActivity

class MainActivity : SDLActivity() {
    companion object {
        @JvmStatic external fun nativeExecuteUiThreadTask(token: Long)
        @JvmStatic external fun nativeHandleActivityResult(requestCode: Int, resultCode: Int, data: Intent?)
        @JvmStatic external fun nativeOnOverlayClosed(handle: Long)

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
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        nativeHandleActivityResult(requestCode, resultCode, data)
    }
}
