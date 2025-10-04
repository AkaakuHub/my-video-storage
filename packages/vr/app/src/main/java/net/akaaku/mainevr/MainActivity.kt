package net.akaaku.mainevr

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.view.Surface
import android.widget.TextView
import androidx.media3.common.MediaItem
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.DefaultLoadControl
import net.akaaku.mainevr.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private var player: ExoPlayer? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Example of a call to a native method
        binding.sampleText.text = stringFromJNI()

        // Initialize ExoPlayer
        initializePlayer()
    }

    private fun initializePlayer() {
        val loadControl = DefaultLoadControl.Builder()
           .setBufferDurationsMs(60000, 60000, 1500, 5000)
           .build()

        player = ExoPlayer.Builder(this)
           .setLoadControl(loadControl)
           .build()

        // サーバーのTailscale IPを指定
        // いったんlocalで
        val serverIp = "192.168.1.11"
        val mediaItem = MediaItem.fromUri("http://$serverIp:3001/stream/my_video/playlist.m3u8")
        player?.setMediaItem(mediaItem)
        player?.prepare()
        player?.playWhenReady = true
    }

    private fun releasePlayer() {
        player?.release()
        player = null
    }

    override fun onStart() {
        super.onStart()
        if (player == null) {
            initializePlayer()
        }
    }

    override fun onResume() {
        super.onResume()
        player?.playWhenReady = true
    }

    override fun onPause() {
        super.onPause()
        player?.playWhenReady = false
    }

    override fun onStop() {
        super.onStop()
        releasePlayer()
    }

    /**
     * A native method that is implemented by the 'mainevr' native library,
     * which is packaged with this application.
     */
    external fun stringFromJNI(): String

    // JNI interface methods for OpenXR integration
    external fun nativeOnCreate()
    external fun nativeOnResume()
    external fun nativeOnPause()
    external fun nativeOnDestroy()
    external fun nativeSetSurface(surface: Surface)
    external fun nativeGetTextureId(): Int

    companion object {
        // Used to load the 'mainevr' library on application startup.
        init {
            System.loadLibrary("mainevr")
        }
    }
}