package dev.atsushieno.uapmd

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Base64
import android.util.Log
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

class AutomationReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        executor.execute {
            try {
                Log.i(TAG, "AutomationReceiver: received ${intent.action} broadcast")

                if (intent.action == ACTION_GET_JS_JOB) {
                    val jobId = intent.getStringExtra(EXTRA_JOB_ID)
                    if (jobId.isNullOrBlank()) {
                        val message = "Missing job id. Use --es $EXTRA_JOB_ID '...'."
                        Log.e(TAG, message)
                        pending.setResultCode(5)
                        pending.setResultData(message)
                        return@execute
                    }
                    val result = MainActivity.nativeGetAutomationScriptJob(jobId)
                    Log.i(TAG, "AutomationReceiver: job $jobId -> $result")
                    pending.setResultCode(0)
                    pending.setResultData(result)
                    return@execute
                }

                if (intent.action == ACTION_CLEAR_JS_JOB) {
                    val jobId = intent.getStringExtra(EXTRA_JOB_ID)
                    if (jobId.isNullOrBlank()) {
                        val message = "Missing job id. Use --es $EXTRA_JOB_ID '...'."
                        Log.e(TAG, message)
                        pending.setResultCode(5)
                        pending.setResultData(message)
                        return@execute
                    }
                    MainActivity.nativeClearAutomationScriptJob(jobId)
                    val result = """{"jobId":"$jobId","cleared":true}"""
                    Log.i(TAG, "AutomationReceiver: cleared job $jobId")
                    pending.setResultCode(0)
                    pending.setResultData(result)
                    return@execute
                }

                val activity = MainActivity.getInstance()
                if (activity == null) {
                    val message = "uapmd-app is not running. Launch the app before sending JS automation."
                    Log.e(TAG, message)
                    pending.setResultCode(2)
                    pending.setResultData(message)
                    return@execute
                }

                val code = when {
                    intent.hasExtra(EXTRA_CODE) -> intent.getStringExtra(EXTRA_CODE)
                    intent.hasExtra(EXTRA_CODE_BASE64) -> {
                        val encoded = intent.getStringExtra(EXTRA_CODE_BASE64)
                        if (encoded.isNullOrEmpty()) null
                        else String(Base64.decode(encoded, Base64.DEFAULT), Charsets.UTF_8)
                    }
                    else -> null
                }

                if (code.isNullOrBlank()) {
                    val message = "Missing JS payload. Use --es $EXTRA_CODE '...' or --es $EXTRA_CODE_BASE64 '...'."
                    Log.e(TAG, message)
                    pending.setResultCode(3)
                    pending.setResultData(message)
                    return@execute
                }

                when (intent.action) {
                    ACTION_RUN_JS -> {
                        Log.i(TAG, "AutomationReceiver: dispatching JS payload (${code.length} chars)")
                        val result = MainActivity.nativeRunAutomationScript(code)
                        Log.i(TAG, "JS result: $result")
                        pending.setResultCode(0)
                        pending.setResultData(result)
                    }
                    ACTION_RUN_JS_ASYNC -> {
                        Log.i(TAG, "AutomationReceiver: starting async JS job (${code.length} chars)")
                        val jobId = MainActivity.nativeStartAutomationScriptJob(code)
                        pending.setResultCode(0)
                        pending.setResultData(jobId)
                    }
                    else -> {
                        val message = "Unsupported action: ${intent.action}"
                        Log.e(TAG, message)
                        pending.setResultCode(4)
                        pending.setResultData(message)
                    }
                }
            } catch (t: Throwable) {
                val message = "Failed to run JS automation: ${t.javaClass.name}: ${t.message ?: ""}".trim()
                Log.e(TAG, message, t)
                pending.setResultCode(1)
                pending.setResultData(message)
            } finally {
                pending.finish()
            }
        }
    }

    companion object {
        const val ACTION_RUN_JS = "dev.atsushieno.uapmd.RUN_JS"
        const val ACTION_RUN_JS_ASYNC = "dev.atsushieno.uapmd.RUN_JS_ASYNC"
        const val ACTION_GET_JS_JOB = "dev.atsushieno.uapmd.GET_JS_JOB"
        const val ACTION_CLEAR_JS_JOB = "dev.atsushieno.uapmd.CLEAR_JS_JOB"
        const val EXTRA_CODE = "code"
        const val EXTRA_CODE_BASE64 = "code_base64"
        const val EXTRA_JOB_ID = "job_id"
        private const val TAG = "uapmd-adb"
        private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    }
}
