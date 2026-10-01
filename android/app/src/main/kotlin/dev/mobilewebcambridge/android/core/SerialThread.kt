package dev.mobilewebcambridge.android.core

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/**
 * A named thread with a FIFO task queue. Every component confines its mutable state to exactly one
 * of these, so no component needs locks; other threads hand work over with [post], which keeps
 * submission order.
 */
class SerialThread(name: String, priority: Int = Process.THREAD_PRIORITY_DEFAULT) {
    private val thread = HandlerThread(name, priority).apply { start() }

    val handler: Handler = Handler(thread.looper)
    val executor: Executor = Executor { task -> handler.post(task) }
    val name: String get() = thread.name

    val isCurrent: Boolean get() = Looper.myLooper() == thread.looper

    fun post(task: () -> Unit) {
        handler.post(task)
    }

    fun postDelayed(delayMs: Long, task: Runnable) {
        handler.postDelayed(task, delayMs)
    }

    fun cancel(task: Runnable) {
        handler.removeCallbacks(task)
    }

    /** Runs [task] on this thread and waits for its result (setup and teardown only). */
    fun <T> call(timeoutMs: Long = 5_000, task: () -> T): T {
        if (isCurrent) return task()
        var result: Result<T>? = null
        val done = CountDownLatch(1)
        val posted = handler.post {
            result = runCatching(task)
            done.countDown()
        }
        check(posted) { "$name has quit" }
        check(done.await(timeoutMs, TimeUnit.MILLISECONDS)) { "Timed out waiting for $name" }
        return checkNotNull(result).getOrThrow()
    }

    fun checkCurrent() {
        check(isCurrent) { "Must run on $name, not ${Thread.currentThread().name}" }
    }

    fun quit() {
        thread.quitSafely()
    }
}
