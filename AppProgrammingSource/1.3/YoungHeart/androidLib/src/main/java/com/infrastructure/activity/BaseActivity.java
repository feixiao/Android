package com.infrastructure.activity;

import android.app.Activity;
import android.os.Bundle;

public abstract class BaseActivity extends Activity {

	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		
		initVariables();
		initViews(savedInstanceState);
		loadData();
	}

	// 子类分别实现
	// initVariables ：初始化变量，包括 Intent 带的数据和 Activity 内的变量。
	protected abstract void initVariables();
	// initViews：加载 layout 布局文件，初始化控件，为控件挂上事件方法。
	protected abstract void initViews(Bundle savedInstanceState);
	// loadData：调用 MobileAPI 获取数据。
	protected abstract void loadData();
}