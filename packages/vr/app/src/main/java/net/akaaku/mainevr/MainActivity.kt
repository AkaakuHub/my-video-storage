// このファイルは、あなたの現在のコードから不要な initializeOpenGLContext を削除し、
// ライフサイクル呼び出しを整理したものです。
// あなたの再試行ロジックは素晴らしいので、そのまま活用します。

package net.akaaku.mainevr

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.view.Surface
import android.graphics.SurfaceTexture
import androidx.media3.common.MediaItem
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.DefaultLoadControl
import net.akaaku.mainevr.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private var player: ExoPlayer? = null
    private var surfaceTexture: android.graphics.SurfaceTexture? = null
    private var videoSurface: Surface? = null
    private var isFrameAvailable = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.sampleText.text = stringFromJNI()
        nativeOnCreate()
    }

    override fun onResume() {
        super.onResume()
        nativeOnResume()
        if (player == null) {
            initializePlayer()
        }
        player?.playWhenReady = true
        binding.sampleText.text = "Starting VR session and video bridge..."
        setupVideoTextureBridge()
    }

    override fun onPause() {
        super.onPause()
        player?.playWhenReady = false
        nativeOnPause()
    }

    override fun onDestroy() {
        super.onDestroy()
        releasePlayer()
        releaseVideoSurface()
        nativeOnDestroy()
    }

    private fun setupVideoTextureBridge() {
        initializeVideoBridgeWithRetry()
    }

    private fun initializePlayer() {
        val loadControl = DefaultLoadControl.Builder()
       .setBufferDurationsMs(60000, 60000, 1500, 5000)
       .build()
        player = ExoPlayer.Builder(this)
       .setLoadControl(loadControl)
       .build()
        val testVideoUrl = "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4"
        val mediaItem = MediaItem.fromUri(testVideoUrl)
        player?.setMediaItem(mediaItem)
        player?.prepare()
        binding.sampleText.text = "Player initialized (waiting for video bridge)"
        android.util.Log.d("MaineVR", "Player initialized successfully")
    }

    private fun initializeVideoBridgeWithRetry() {
        val retryHandler = android.os.Handler(android.os.Looper.getMainLooper())
        val retryRunnable = object : Runnable {
            override fun run() {
                val glTextureId = nativeGetTextureId()
                if (glTextureId > 0) {
                    binding.sampleText.text = "✅ Texture created after retry! ID: $glTextureId"
                    android.util.Log.i("MaineVR", "Texture created after retry! ID: $glTextureId")
                    completeVideoBridgeSetup(glTextureId)
                } else {
                    binding.sampleText.text = "Retrying texture creation... C++ side not ready"
                    android.util.Log.d("MaineVR", "Texture creation failed (C++ not ready), retrying in 500ms")
                    retryHandler.postDelayed(this, 500)
                }
            }
        }
        retryHandler.postDelayed(retryRunnable, 1000)
    }

    private fun completeVideoBridgeSetup(glTextureId: Int) {
        try {
            surfaceTexture = android.graphics.SurfaceTexture(glTextureId).apply {
                setOnFrameAvailableListener {
                    isFrameAvailable = true
                }
            }
            videoSurface = Surface(surfaceTexture)
            player?.setVideoSurface(videoSurface)
            binding.sampleText.text = "✅ Video bridge working! Texture: $glTextureId"
            android.util.Log.i("MaineVR", "Video bridge setup complete")
        } catch (e: Exception) {
            binding.sampleText.text = "❌ Bridge setup error: ${e.message}"
            android.util.Log.e("MaineVR", "Bridge setup error", e)
        }
    }

    private fun releasePlayer() {
        player?.release()
        player = null
    }

    private fun releaseVideoSurface() {
        videoSurface?.release()
        videoSurface = null
        surfaceTexture?.release()
        surfaceTexture = null
    }

    external fun stringFromJNI(): String
    external fun nativeOnCreate()
    external fun nativeOnResume()
    external fun nativeOnPause()
    external fun nativeOnDestroy()
    external fun nativeSetSurface(surface: Surface)
    external fun nativeGetTextureId(): Int

    companion object {
        init {
            System.loadLibrary("mainevr")
        }
    }
}