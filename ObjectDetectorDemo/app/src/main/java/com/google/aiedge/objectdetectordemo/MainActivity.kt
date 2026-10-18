package com.google.aiedge.objectdetectordemo

import android.os.Bundle
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import com.google.aiedge.objectdetectordemo.databinding.ActivityMainBinding

/**
 * ObjectDetectorDemo 主 Activity
 * 保持屏幕常亮并作为 Jetpack Navigation 容器承载相机与配置界面。
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // 保持屏幕常亮以确保高帧率连续推流
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }
}
