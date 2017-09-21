package com.example.frank.player01;

import android.os.Environment;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.widget.MediaController;
import android.widget.VideoView;

public class MainActivity extends AppCompatActivity {

    private VideoView videoView;
    private String path;
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        videoView=(VideoView) findViewById(R.id.videoView);

        /**
         * 本地源
         */
        path = Environment.getExternalStorageDirectory().getAbsolutePath() + "/test.mp4";
        Log.i("path : %s",path);
        videoView.setVideoPath(path);

        /**
         *  网络源
         */

        //videoView.setVideoURI();

        /**
         * MideaControl
         */

        MediaController controller = new MediaController(this);

        /**
         * 设置VideoView和MediaController的关联关系
         */
        videoView.setMediaController(controller);

        /**
         * 设置MediaController和VideoView的关联关系
         */
        controller.setMediaPlayer(videoView);


    }
}