package dev.mobilewebcambridge.android.gl

import android.opengl.GLES11Ext
import android.opengl.GLES20
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * GLES 2 drawing into the encoder surface: camera frames from an external (OES) texture, and a
 * synthetic test pattern. Must be used on the thread that owns the EGL context.
 */
class FrameRenderer(private val width: Int, private val height: Int) {
    private val program: Int = linkProgram(VERTEX_SHADER, FRAGMENT_SHADER)
    private val positionLocation = GLES20.glGetAttribLocation(program, "aPosition")
    private val texCoordLocation = GLES20.glGetAttribLocation(program, "aTexCoord")
    private val textureLocation = GLES20.glGetUniformLocation(program, "uTexture")
    private val positions: FloatBuffer = floatBuffer(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f))
    private val texCoords: FloatBuffer = floatBuffer(FloatArray(8))

    /** Creates the external texture a camera `SurfaceTexture` renders into. */
    fun createExternalTexture(): Int {
        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, ids[0])
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        return ids[0]
    }

    fun deleteTexture(textureId: Int) {
        GLES20.glDeleteTextures(1, intArrayOf(textureId), 0)
    }

    /** Draws [textureId] with per-corner texture coordinates (see `TextureMapping`). */
    fun drawExternal(textureId: Int, cornerTexCoords: FloatArray) {
        GLES20.glViewport(0, 0, width, height)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glUniform1i(textureLocation, 0)

        positions.position(0)
        GLES20.glEnableVertexAttribArray(positionLocation)
        GLES20.glVertexAttribPointer(positionLocation, 2, GLES20.GL_FLOAT, false, 0, positions)

        texCoords.position(0)
        texCoords.put(cornerTexCoords)
        texCoords.position(0)
        GLES20.glEnableVertexAttribArray(texCoordLocation)
        GLES20.glVertexAttribPointer(texCoordLocation, 2, GLES20.GL_FLOAT, false, 0, texCoords)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(positionLocation)
        GLES20.glDisableVertexAttribArray(texCoordLocation)
    }

    /**
     * SMPTE-like colour bars with a bar sweeping once per second, a white square flashing for the
     * first 100 ms of every second (handy for measuring glass-to-glass latency) and a 16-bit binary
     * frame counter along the top edge (white = 1), so dropped or repeated frames show on the host.
     */
    fun drawTestPattern(timestampUs: Long, frameIndex: Long) {
        GLES20.glViewport(0, 0, width, height)
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST)
        GLES20.glClearColor(0.08f, 0.09f, 0.12f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glEnable(GLES20.GL_SCISSOR_TEST)

        val barWidth = width / BARS.size
        val barHeight = height * 2 / 3
        BARS.forEachIndexed { index, color ->
            fillRect(index * barWidth, height - barHeight, barWidth, barHeight, color)
        }
        val phase = (timestampUs % 1_000_000L) / 1_000_000f
        val sweepWidth = (width / 48).coerceAtLeast(4)
        fillRect(((width - sweepWidth) * phase).toInt(), 0, sweepWidth, height - barHeight, WHITE)
        if (timestampUs % 1_000_000L < 100_000L) {
            val square = height / 6
            fillRect(width - square - square / 4, square / 4, square, square, WHITE)
        }
        val cell = (width / 32).coerceAtLeast(8)
        for (bit in 0 until 16) {
            val x = bit * cell
            if (x + cell > width) break
            val set = ((frameIndex shr (15 - bit)) and 1L) == 1L
            fillRect(x, height - cell, cell, cell, if (set) WHITE else BLACK)
        }
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST)
    }

    fun release() {
        GLES20.glDeleteProgram(program)
    }

    private fun fillRect(x: Int, y: Int, w: Int, h: Int, color: FloatArray) {
        GLES20.glScissor(x, y, w, h)
        GLES20.glClearColor(color[0], color[1], color[2], 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
    }

    private companion object {
        val WHITE = floatArrayOf(1f, 1f, 1f)
        val BLACK = floatArrayOf(0f, 0f, 0f)
        val BARS = arrayOf(
            floatArrayOf(0.75f, 0.75f, 0.75f),
            floatArrayOf(0.75f, 0.75f, 0f),
            floatArrayOf(0f, 0.75f, 0.75f),
            floatArrayOf(0f, 0.75f, 0f),
            floatArrayOf(0.75f, 0f, 0.75f),
            floatArrayOf(0.75f, 0f, 0f),
            floatArrayOf(0f, 0f, 0.75f),
        )

        val VERTEX_SHADER = """
            attribute vec4 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = aTexCoord;
            }
        """.trimIndent()

        // trimIndent keeps the #extension directive at the very start of the source, where some
        // GLSL compilers insist on finding it.
        val FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES uTexture;
            void main() {
                gl_FragColor = texture2D(uTexture, vTexCoord);
            }
        """.trimIndent()

        fun floatBuffer(values: FloatArray): FloatBuffer =
            ByteBuffer.allocateDirect(values.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
                put(values)
                position(0)
            }

        fun compileShader(type: Int, source: String): Int {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, source.trimIndent())
            GLES20.glCompileShader(shader)
            val status = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
            if (status[0] == 0) {
                val log = GLES20.glGetShaderInfoLog(shader)
                GLES20.glDeleteShader(shader)
                error("Shader compilation failed: $log")
            }
            return shader
        }

        fun linkProgram(vertexSource: String, fragmentSource: String): Int {
            val vertex = compileShader(GLES20.GL_VERTEX_SHADER, vertexSource)
            val fragment = compileShader(GLES20.GL_FRAGMENT_SHADER, fragmentSource)
            val program = GLES20.glCreateProgram()
            GLES20.glAttachShader(program, vertex)
            GLES20.glAttachShader(program, fragment)
            GLES20.glLinkProgram(program)
            GLES20.glDeleteShader(vertex)
            GLES20.glDeleteShader(fragment)
            val status = IntArray(1)
            GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
            if (status[0] == 0) {
                val log = GLES20.glGetProgramInfoLog(program)
                GLES20.glDeleteProgram(program)
                error("Program link failed: $log")
            }
            return program
        }
    }
}
