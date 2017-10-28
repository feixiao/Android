package com.example.frank.activitytest;

import android.content.Intent;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.widget.Toast;

public class SencondActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_sencond);
        Intent intent = getIntent();

        // 获取FirstActivity传递过来的数据
        String data = intent.getStringExtra("extra_data");
        Toast.makeText(SencondActivity.this,data, Toast.LENGTH_SHORT).show();

    }
}
