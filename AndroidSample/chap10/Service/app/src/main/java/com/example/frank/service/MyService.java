package com.example.frank.service;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.support.annotation.IntDef;
import android.util.Log;

public class MyService extends Service {
    private static final String TAG = "MyService";
    private DownloadBinder mBinder = new DownloadBinder();

    public MyService() {
    }

    @Override
    public IBinder onBind(Intent intent){
        return mBinder;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Log.d(TAG,"onCreate executed");
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.d(TAG,"onDestroy executed");
    }

    @Override
    public int onStartCommand(Intent intent,int flags, int startId) {
        Log.d(TAG,"onStartCommand executed");
        return super.onStartCommand(intent, flags, startId);
    }

    public  class DownloadBinder  extends Binder {
        public void startDownload() {
            Log.d(TAG,"startDownload executed");
        }

        public int getProgress() {
            Log.d(TAG,"getProgress executed");
            return 0;
        }
    }


}
