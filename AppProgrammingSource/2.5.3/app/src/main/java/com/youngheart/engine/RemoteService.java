package com.youngheart.engine;

import java.util.List;

import com.alibaba.fastjson.JSON;
import com.infrastructure.activity.BaseActivity;
import com.infrastructure.net.DefaultThreadPool;
import com.infrastructure.net.HttpRequest;
import com.infrastructure.net.RequestCallback;
import com.infrastructure.net.RequestParameter;
import com.infrastructure.net.Response;
import com.infrastructure.net.URLData;
import com.infrastructure.net.UrlConfigManager;
import com.youngheart.mockdata.MockService;

/**
 * RemoteService : 这个是单例是用来发起请求的，它会创建一个 request，并将其添加到RequestManager 中，
 * 然后放到 DefaultThreadPool 的一个线程中去执行这个 request。
 * */
public class RemoteService {
	private static RemoteService service = null;

	private RemoteService() {

	}

	public static synchronized RemoteService getInstance() {
		if (RemoteService.service == null) {
			RemoteService.service = new RemoteService();
		}
		return RemoteService.service;
	}
	// 3: 构建RemoteService，使用invoke发送请求
	public void invoke(final BaseActivity activity, 
			final String apiKey,
			final List<RequestParameter> params, 
			final RequestCallback callBack) {

		final URLData urlData = UrlConfigManager.findURL(activity, apiKey);
		if (urlData.getMockClass() != null) {
			try {
				MockService mockService = (MockService) Class.forName(
						urlData.getMockClass()).newInstance();
				String strResponse = mockService.getJsonData();

				final Response responseInJson = JSON.parseObject(strResponse,
						Response.class);
				if (callBack != null) {
					if (responseInJson.hasError()) {
						callBack.onFail(responseInJson.getErrorMessage());
					} else {
						callBack.onSuccess(responseInJson.getResult());
					}
				}
			} catch (ClassNotFoundException e) {
				e.printStackTrace();
			} catch (InstantiationException e) {
				e.printStackTrace();
			} catch (IllegalAccessException e) {
				e.printStackTrace();
			}
		} else {
			// 2： 创建请求
			HttpRequest request = activity.getRequestManager().createRequest(
					urlData, params, callBack);

			DefaultThreadPool.getInstance().execute(request);
		}
	}
}