﻿#include "stdafx.h"
#include "GlobalViewGateway.h"
#include "IStackWalk.h"
#include "ViewGateway.h"
#include "MngMsgRegister.h"
#include "ViewSession.h"
#include "ViewContainer.h"
#include "ClientContainer.h"
#include "ClientSession.h"
#include "AuthMng.h"


/// purpose: 构造函数
GlobalViewGateway::GlobalViewGateway():
	m_MngConnector(m_MngProcesser, m_MngBr, m_TimerAxis)
{
	setGlobal(static_cast<IGlobalViewGateway*>(this));

	m_dwControlCode = 0;	// 当前操作代码

	memset(szTraceFileName,0,sizeof(szTraceFileName));
	memset(szCrashFileName,0,sizeof(szCrashFileName));
	memset(szDumpFileName,0,sizeof(szDumpFileName));
	memset(m_szWorkDir,0,sizeof(m_szWorkDir));
	memset(m_szServerBinWorkDir,0,sizeof(m_szServerBinWorkDir));

	m_ManagerConnector = NULL;

	// 是否初始化
	m_bInit = false;
	// 是否启动
	m_bStart = false;

	m_bUpdate = false;
	m_bNewDataCome = false;

	m_dwRecvSpeedServer = 0;		// 服务器收包速度
	m_dwSendSpeedServer = 0;		// 服务器发包速度
	m_dwRecvSpeedClient = 0;		// 客户端收包速度
	m_dwSendSpeedClient = 0;		// 客户端发包速度

	// 注册消息
	MngMsgRegister::RegisterMsgs(m_MngProcesser);
}

/// purpose: 析构函数
GlobalViewGateway::~GlobalViewGateway()
{
	Close();
	UnInitEnvironment();	// 反初始化环境
	setGlobal(NULL);
}

// purpose: 初始化启动环境
bool GlobalViewGateway::InitEnvironment(void)
{
	if (IsInit())
	{
		return false;
	}

	// dump
#ifdef SUPPORT_STACKWALK
	IStackWalk * pStackWalk = createStackWalkProc();
	if(pStackWalk == NULL)
	{
		return false;
	}	

	// dump类型
	pStackWalk->setDumpType(MiniDumpWithFullMemory);

	// dump文件
	pStackWalk->setLogFileName(my_utf82t(szCrashFileName));
	pStackWalk->setDumpFileName(my_utf82t(szDumpFileName));
#endif

	/// 获取物理处理器个数（如双核就有2个）
	DWORD dwCPUCounts = getPhysicalProcessorCount();
	DWORD dwThreadNum = 8;	// 默认网络8个线程处理收发
	// 如果CPU个数多于4个就加多
	if (dwCPUCounts>4)
	{
		dwThreadNum = 2*dwCPUCounts + 8;	// 每个CPU跑2个线程
	}

	// 初始化网络层
	if ( InitializeNetwork(dwThreadNum,true)<0 )
	{
		ErrorLn(_GT("初始化网络失败，请检查配置"));
		return false;
	}

	TraceLn(_GT("CPU个数:")<<dwCPUCounts);
	TraceLn(_GT("网络层线程数:") << dwThreadNum);

	// 挂接事件
	HANDLE hNetEvent = GetNetworkEventHandle();
	GetAsynIoReactor()->AddEvent(hNetEvent);
	GetAsynIoReactor()->RegisterEventHandler(hNetEvent,this);

	m_hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
	LARGE_INTEGER DueTime;
	DueTime.QuadPart = 0;
	SetWaitableTimer(m_hTimer, &DueTime, 33, NULL, NULL, TRUE);

	GetAsynIoReactor()->AddEvent(m_hTimer);
	GetAsynIoReactor()->RegisterEventHandler(m_hTimer,this);

	// 是否初始化
	m_bInit = true;

	// 启动管理连接器
	CreateManagerConnector();

	return true;
}


// purpose: 反初始化环境
bool GlobalViewGateway::UnInitEnvironment(void)
{
	if (!IsInit())
	{
		return false;
	}

	GetAsynIoReactor()->RemoveEvent(GetNetworkEventHandle());

	if ( m_hTimer!=INVALID_HANDLE )
	{
		GetAsynIoReactor()->RemoveEvent(m_hTimer);
		CloseHandle(m_hTimer);
		m_hTimer = INVALID_HANDLE;
	}

	//UninitializeNetwork();

	m_bInit = false;

	return true;
}

// 释放所有Listener
void GlobalViewGateway::ReleaseListeners()
{
	m_ClientListener.Release();
	m_ViewListener.Release();
}

// 通知App实例关闭
void NotifyAppClose()
{
	CWnd * pMain = AfxGetApp()->m_pMainWnd;
	if (pMain != NULL)
	{
		WPARAM wParam = 0;
		LPARAM lParam = 0;
		pMain->SendMessage(WM_MY_CLOSE_APP, wParam, lParam);
	}
}

/**
@name                    : 初始化网络
@brief                   :
@param dwServerID		 : ServerID
@param szServerIP		 : 服务器IP
@param wServerPort		 : 服务器端口
@param wClientListenPort : 客户端监听端口
@return                  :
*/
bool GlobalViewGateway::Create( DWORD dwServerID, const char * szServerIP, WORD wServerPort, WORD wClientListenPort )
{
	try{
		// 是否启动
		if (IsStart())
			return false;

		if( dwServerID <= 0 || dwServerID >= VIEW_MAX_GATEWAYID )
			return false;

		// 创建网络监听
		char szName[64] = {0};
		if ( !m_ClientListener.Create(wClientListenPort,this,DEFAULT_PACK))
		{
			ErrorLn(_GT("监听客户端端口[") << wClientListenPort << _GT("]失败!"));

			// 绑定端口失败,直接关闭,用来程序互斥
			ReleaseListeners();
			// 通知App实例关闭
			NotifyAppClose();

			return false;
		}
		TraceLn(_GT("客户端监听端口:") << wClientListenPort);

		sprintf_s(szName, sizeof(szName), "Client Listen:%d",wClientListenPort);
		m_ClientListener.SetName(szName);

		// ViewServer
		{
			const WORD& dwViewPort = gSetting.m_dwViewPort;

			if ( !m_ViewListener.Create( dwViewPort,this,DEFAULT_PACK))
			{
				ErrorLn(_GT("监听ViewServer端口[") << dwViewPort << _GT("]失败!"));

				// 绑定端口失败,直接关闭,用来程序互斥
				ReleaseListeners();
				// 通知App实例关闭
				NotifyAppClose();
				
				return false;
			}
			TraceLn(_GT("ViewServer监听端口:") << dwViewPort);

			sprintf_s(szName, sizeof(szName), "ViewServer Listen:%d", dwViewPort);
			m_ViewListener.SetName(szName);
		}

		gAuthMng.Init(getTimerAxis());

		// 观战管理服连接器初始化
		m_MngConnector.SetServerID(dwServerID);
		m_MngConnector.Connect( szServerIP, wServerPort );

		SetTimer( ETimerEventID_VoiceControl, 2000, this, "GlobalViewGateway::Create" );
		SetTimer( ETimerEventID_VoiceNetCount, 1000, this, "GlobalViewGateway::Create" );

		m_TotalInfo.dwStartTime		= (DWORD)time(NULL);		// 启动时间
		m_TotalInfo.dwServerPort	= wServerPort;				// 服务器监听端口
		m_TotalInfo.dwClientPort	= wClientListenPort;		// 客户端监听端口
		m_TotalInfo.dwAudioPort		= gSetting.m_dwViewPort;		// 监听ViewServer的端口

		// 是否启动
		m_bStart = true;

		// 通知UI已启动
		CWnd * pMain = AfxGetApp()->m_pMainWnd;
		if (pMain!=NULL)
		{
			WPARAM wParam = 1;
			LPARAM lParam = 0;
			pMain->SendMessage(WM_MY_START_RESULT,wParam,lParam);
		}

		return true;
	}
	catch (...)
	{
		Trace(endl);
		Error("GlobalViewGateway::create, catch exception"<<endl);
	}

	return false;
}

/// purpose: 关闭
bool GlobalViewGateway::Close()
{
#define MyRelease(ptr)		if ((ptr)) {(ptr)->Release(); (ptr) = 0;}

	try{

		// 是否启动
		if (!IsStart())
		{
			return false;
		}

		m_MngConnector.Close();

		// 清除所有用户
		gClientContainer.shutdown();
		gViewContainer.shutdown();
		gAuthMng.Shutdown();

		// 关闭管理连接器
		ReleaseManagerConnector();

		ReleaseListeners();

		// 是否启动
		m_bStart = false;

		// 通知UI已关闭
		CWnd * pMain = AfxGetApp()->m_pMainWnd;
		if (pMain!=NULL)
		{
			WPARAM wParam = 0;
			LPARAM lParam = 0;
			pMain->SendMessage(WM_MY_STOP_RESULT,wParam,lParam);
		}

		return true;
	}
	catch (...)
	{
		Error("GlobalViewGateway::close, catch exception"<<endl);
	}
	return false;
}

/// purpose: 是否初始化
bool GlobalViewGateway::IsInit(void)
{
	return m_bInit;
}

/// purpose: 是否初始化
bool GlobalViewGateway::IsStart(void)
{
	return m_bStart;
}

/// purpose: 关闭服务器程序
void GlobalViewGateway::CloseServerApp(void)
{
	SetControlCode(VS_ControlCode_ShutDown);
}

/// purpose: 启动管理连接器
void GlobalViewGateway::CreateManagerConnector(void)
{
	// 加载服务器管理器连接器设定
	Ini ini("Manager.ini");
	std::string strManagerServerIP = ini.getString("Setting","ManagerServerIP","127.0.0.1");
	int nManagerServerPort = ini.getInt("Setting","ManagerServerPort",8700);

	// 子服务器连接
	TraceLn(_GT("连接子服务器管理器:")<<strManagerServerIP.data()<<":"<<nManagerServerPort);
	m_ManagerConnector = CreateManagerConnectorProc(this,&m_TimerAxis);
	DWORD dwServerID = gSetting.getServerID();
	char szGWName[64] = {0};
	sprintf_s( szGWName, sizeof(szGWName), "VoiceGateway%d", dwServerID );
	m_ManagerConnector->Create(MSG_MG_ENDPOINT_VOICEGATE, (WORD)dwServerID, szGWName );
	if(!m_ManagerConnector->Connect(strManagerServerIP.data(),nManagerServerPort))
	{
		TraceLn(_GT("连接子服务器管理器失败!"));
		return;
	}
	TraceLn(_GT("连接子服务器管理器成功!"));

	// 订阅 要求更新实时数据事件
	m_ManagerConnector->registerEventHandler(EVENT_MG_REAL_TIME_DATA,static_cast<IManagerEventHandler *>(this));
}

/// purpose: 关闭管理连接器
void GlobalViewGateway::ReleaseManagerConnector(void)
{
	if (m_ManagerConnector)
	{
		// 取消订阅 要求更新实时数据事件
		m_ManagerConnector->unregisterEventHandler(EVENT_MG_REAL_TIME_DATA,static_cast<IManagerEventHandler *>(this));
		// 关闭子服务器连接
		TraceLn(_GT("关闭子服务器管理器连接!"));
		m_ManagerConnector->Release();
		m_ManagerConnector = NULL;
	}
}

/// purpose: 设置当前操作代码  DWORD dwCode 参考 EMManagerControlCode
void GlobalViewGateway::SetControlCode(DWORD dwCode)
{
	// 当前操作代码
	m_dwControlCode = dwCode;
}


// purpose: 执行操作代码
bool GlobalViewGateway::DoControlCodeTask(DWORD dwCode)
{
	switch(dwCode)
	{
	case VS_ControlCode_StartService:		// 启动服务
		{
			DWORD dwID = gSetting.getServerID();
			string strServerIP = gSetting.m_strServerIp;
			WORD wServerPort = gSetting.m_dwServerPort;
			WORD wClientListenPort = gSetting.m_dwClientPort;

			if(Create( dwID, strServerIP.c_str(), wServerPort, wClientListenPort ))
			{
				Trace(_GT("启动语音网关 OK ！")<<endl);
			}
			else
			{
				Error(_GT("启动语音网关 失败！")<<endl);
			}
		}
		break;
	case VS_ControlCode_StopService:		// 停止服务
		{
			if(Close())
			{
				Trace(_GT("停止语音网关 OK ！")<<endl);
			}
			else
			{
				Error(_GT("停止语音网关 失败！")<<endl);
			}
		}
		break;
	case VS_ControlCode_ShutDown:		// 关闭程序,停止服务后要关闭进程
		{
			if(Close())
			{
				Trace(_GT("停止语音网关 OK ！")<<endl);
			}
			else
			{
				Error(_GT("停止语音网关 失败！")<<endl);
			}
			// 通知服务器关闭
			CWnd * pMain = AfxGetApp()->m_pMainWnd;
			if (pMain!=NULL)
			{
				WPARAM wParam = 0;
				LPARAM lParam = 0;
				pMain->SendMessage(WM_MY_CLOSE_APP,wParam,lParam);
			}
		}
		break;
	default:
		{
			ErrorLn(_GT("尚有一个执行操作代码未处理，dwCode = ")<<dwCode);
		}
		break;
	}
	return true;
}


////////// IManagerEventHandler 处理子服务器连接器执行事件 ///////////////////////////////
/** 处理子服务器连接器执行事件
@param   wEventID ：事件ID
@param   pszContext : 上下文
@param   nLen : 上下文长度
@retval buffer 
*/
void GlobalViewGateway::OnManagerEventExecute(WORD wEventID, LPSTR pszContext, size_t nLen)
{
	if (NULL==m_ManagerConnector)
	{
		return;
	}

	switch(wEventID)
	{
	case EVENT_MG_REAL_TIME_DATA:		// 要求更新实时数据事件
		{
			/// 获取物理处理器个数（如双核就有2个）
			static size_t dwCPUCounts = 0;
			if (dwCPUCounts==0)
			{
				dwCPUCounts = getPhysicalProcessorCount();
			}

			/// 获得当前进程的CPU占用百分比
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_CPUCurrentUsedValue,(int)(MANAGER_REALTIMEDATA_CPU_MAX*dwCPUCounts*getCPUCurrentUsedValueByCurrentProcess()));
			/// 获取当前进程的物理内存已使用大小
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_PhysicalMemoryUsedSize,(int)getPhysicalMemoryUsedSizeByCurrentProcess());
			/// 获取当前进程的虚拟内存已使用大小
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_VirtualMemoryUsedSize,(int)getVirtualMemoryUsedSizeByCurrentProcess());
			// 更新实时数据 在线数
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_OnLineCounts,(int)(gClientContainer.Count()));
			// 客户端每秒发包速度
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_ClientSendSpeed,(int)(m_TotalInfo.dwSendSpeedClient));
			// 客户端每秒收包速度
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_ClientRecvSpeed,(int)(m_TotalInfo.dwRecvSpeedClient));
			// 服务器每秒发包速度
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_ServerSendSpeed,(int)(m_TotalInfo.dwSendSpeedServer));
			// 服务器每秒收包速度
			m_ManagerConnector->SetRealTimeData(MG_VoiceGatewayRTData_ServerRecvSpeed,(int)(m_TotalInfo.dwRecvSpeedServer));
		}
		break;
	default:
		{
			ErrorLn(_GT("尚有一个子服务器连接器执行事件订阅了但未处理，wEventID = ")<<wEventID);
		}
		break;
	}
}

/**
@name         : 处理其他服务器通过子服务器转发的消息
@brief        : 具体内容不清楚
@param actionId  : 消息码
@param head  : 消息头
@param data  : 消息内容
@param len:消息长度
*/
void GlobalViewGateway::HandleManagerMessage(ulong actionId, SManagerMsgHead* head, LPSTR data, size_t len)
{
	
	switch(actionId)
	{
	case MSG_MG_HANDSHAKE_RESPONSE:		//  子服务器连接成功回应握手消息
		{
			if (m_ManagerConnector)
			{
				m_ManagerConnector->SetServerState(MG_SERVERSTATE_RUNING);// 已启动，正在远行
			}
		}
		break;
	case MSG_MG_REQUESTSTART_RESULT:	// 软件服务器向桥服务器请求启动结果消息,调用 RequestStart()后收到
		{
			if(data==NULL|| len != sizeof(SMsgManagerRequestStartResult_M))
			{
				ErrorLn(_GT("向桥服务器请求启动结果数据出错,数据有误！"));
				return;
			}

			SMsgManagerRequestStartResult_M * pMsg = (SMsgManagerRequestStartResult_M *)(data);

			WarningLn(_GT("请求启动，结果代码=")<<pMsg->dwResultFlag<<_GT(",启动标识=")<<pMsg->dwResultFlag<<_GT(",用户数据=")<<pMsg->dwUserData<<_GT(",结果信息=")<<pMsg->szResultMsg);


			if (pMsg->bResult==false)
			{
				ErrorLn(_GT("请求启动失败，结果代码=")<<pMsg->dwResultFlag<<_GT(",启动标识=")<<pMsg->dwResultFlag<<_GT(",用户数据=")<<pMsg->dwUserData<<_GT(",结果信息=")<<pMsg->szResultMsg);

				// 正在加载脚本数据,要脚本的可定时重试请求启动,直到取得
				if (pMsg->dwResultFlag==MANAGER_STARTCODE_LOADSCRIPT)
				{
					// 定时重试请求启动 调用 RequestStart()
					// todotodo
				}
			} 
			else
			{
				//启动服务器
				// todotodo
			}
		}
		break;
	case MSG_MG_SENDSCRIPTDATA:		// 收到服务器启动脚本文件数据
		{
			if(data==NULL|| len < sizeof(SMsgManagerSendScriptData_RM))
			{
				ErrorLn(_GT("收到服务器启动脚本文件数据出错,数据有误！"));
				return;
			}

			SMsgManagerSendScriptData_RM * pMsg = (SMsgManagerSendScriptData_RM *)(data);

			TraceLn(_GT("收到服务器启动脚本文件数据，游戏世界ID=")<<pMsg->dwWorldID<<_GT(",服务器类型=")<<pMsg->nServerType<<_GT(",脚本文件组ID=")<<pMsg->dwScriptGroupID<<_GT(",最大配置号=")<<pMsg->nMaxSchemeIndex<<",配置号="<<pMsg->nSchemeIndex<<_GT(",本配置号数据长度=")<<pMsg->dwDataLens);

			if (pMsg->nServerType!=MSG_MG_ENDPOINT_ROOT)
			{
				ErrorLn(_GT("服务器类型[")<<pMsg->nServerType<<_GT("]与本服务器[")<<MSG_MG_ENDPOINT_ROOT<<_GT("]不符!"));
				return;
			}

			// 脚本文件数据,本配置号数据长度 pMsg->dwDataLens
			LPSTR pDataBuffer = (LPSTR)(data+sizeof(SMsgManagerSendScriptData_RM));

			// 加入文件系统中
			// todotodo

		}
		break;
	case MSG_MG_CLOSESERVER:		// 要求软件服务器关闭
		{
			if(data==NULL|| len != sizeof(SMsgManagerCloseServer_Z))
			{
				ErrorLn(_GT("要求软件服务器关闭出错,数据有误！"));
				return;
			}

			SMsgManagerCloseServer_Z * pMsg = (SMsgManagerCloseServer_Z *)(data);

			// 关闭服务器
			// todotodo
			WarningLn(_GT("请求关闭，用户ID=")<<pMsg->dwUserID<<_GT(",软件服务器UID=")<<pMsg->dwServerUID<<_GT(",服务器类型=")<<pMsg->dwServerType<<_GT(",第n号=")<<pMsg->dwSubID<<_GT(",关闭标识=")<<pMsg->dwCloseFlag<<",延迟关闭毫秒数="<<pMsg->dwDelays);

			// 请求关闭,如果没有请求，就关了服务器，会当作当机处理
			m_ManagerConnector->RequestClose();

			/// 关闭服务器程序
			CloseServerApp();

		}
		break;
	case MSG_MG_SERVERSENDAD:		// 要求软件服务器发布公告
		{
			if(data==NULL|| len < sizeof(SMsgManagerServerSendAd_Z))
			{
				ErrorLn(_GT("要求软件服务器发布公告出错,数据有误！"));
				return;
			}

			SMsgManagerServerSendAd_Z * pMsg = (SMsgManagerServerSendAd_Z *)(data);

			// 公告信息
			char szSendAdMsg[512] = {0};

			char * pStrSendAd = data+sizeof(SMsgManagerServerSendAd_Z);
			DWORD dwMsgStrLens = pMsg->nSendMsgLens +1;
			if (dwMsgStrLens>sizeof(szSendAdMsg))
			{
				dwMsgStrLens = sizeof(szSendAdMsg);
			}
			if (dwMsgStrLens>0)
			{
				sstrcpyn(szSendAdMsg, pStrSendAd, dwMsgStrLens);		// 公告信息
			}

			// 发布公告
			// todotodo
			WarningLn(_GT("发布公告，用户ID=")<<pMsg->dwUserID<<_GT(",软件服务器UID=")<<pMsg->dwServerUID<<_GT(",服务器类型=")<<pMsg->dwServerType<<_GT(",第n号=")<<pMsg->dwSubID<<_GT(",标识=")<<pMsg->dwSendFlag<<_GT(",延迟关闭毫秒数=")<<pMsg->dwDelays<<_GT(",公告=")<<szSendAdMsg);
		}
		break;
	case MSG_MG_SHOWWINDOW:		// 通知服务器显示程序窗口
		{
			if(data==NULL)
			{
				ErrorLn(_GT("通知服务器显示程序窗口出错,数据有误！"));
				return;
			}

			SMsgManagerShowServerWindow_Z * pMsg = (SMsgManagerShowServerWindow_Z *)(data);

			WarningLn(_GT("通知服务器显示程序窗口，标识=")<<pMsg->dwFlag);

			// 通知服务器关闭
			CWnd * pMain = AfxGetApp()->m_pMainWnd;
			if (pMain!=NULL)
			{
				if (pMsg->dwFlag==0)
				{
					ShowWindowAsync(pMain->m_hWnd,SW_HIDE);
				}
				else
				{
					ShowWindowAsync(pMain->m_hWnd,SW_SHOWNORMAL);
				}
			}
		}
		break;
	case MSG_MG_CONTROLSOFTSERVER:		// 操作软件服务器消息
		{
			if(len < sizeof(SMsgManagerControlSoftServer_C))
			{
				break;
			}

			SMsgManagerControlSoftServer_C * pMsg = (SMsgManagerControlSoftServer_C *)(data);

			switch(pMsg->dwControlCode)
			{
			case MG_SoftControl_SetServerUID:	// 设定软件服务器的UID
				{
					// m_ManagerConnector->GetServerUID();	取得在桥服务器上的UID
					// m_ManagerConnector->GetWorldID();	取得游戏世界ID
					// 管理连接器已更新游戏世界ID,在这里可处理相关事件处理

					if (m_ManagerConnector)
					{
						const SGameWorldConnectorInfo *pWorldInfo = m_ManagerConnector->GetGameWorldInfo();

						m_TotalInfo.bIsPublic		= isPublicGameWorld();			// 是否公共游戏世界
						m_TotalInfo.dwGameID		= pWorldInfo->dwGameID;			// 游戏ID
						m_TotalInfo.dwAreaID		= pWorldInfo->dwAreaID;			// 游戏区域ID
						m_TotalInfo.dwWorldID		= pWorldInfo->dwWorldID;		// 游戏世界ID
						// 游戏名称
						sstrcpyn(m_TotalInfo.szGameName, pWorldInfo->szGameName, sizeof(m_TotalInfo.szGameName));
						// 游戏区域名称
						sstrcpyn(m_TotalInfo.szAreaName, pWorldInfo->szAreaName, sizeof(m_TotalInfo.szAreaName));
						// 游戏世界名称
						sstrcpyn(m_TotalInfo.szWorldName, pWorldInfo->szWorldName, sizeof(m_TotalInfo.szWorldName));
					}
				}
				break;
			}
		}
		break;
	default:
		{
			ErrorLn(_GT("尚有一个服务器管理消息未处理，actionId = ")<<actionId);
		}
		break;
	}
}



///////////////////////// IAcceptorHandler ///////////////////////////////////
void GlobalViewGateway::OnAccept( WORD wListenPort,IConnection * pIncomingConn,IConnectionEventHandler ** ppHandler )
{
	if (wListenPort==m_ClientListener.GetListenPort() )
	{
		ClientSession* pNewUser = gClientContainer.CreateUser(pIncomingConn, m_TimerAxis);

		// 如果语音网关未与语音服务器握手完毕，不接受连接
		if( !m_MngConnector.IsReady())
		{
			pNewUser->Release();
			return ;
		}

		if (gClientContainer.IsFull())
		{
			ErrorLn(_GT("Client Sessions is Full!"));
			pNewUser->Release();
			return;
		}

		*ppHandler = pNewUser;

		char szName[64] = {0};
		sprintf_s(szName, sizeof(szName), "观战Client Pointer[%d]",pNewUser);
		pIncomingConn->setName(szName);

	}
	else if( wListenPort == m_ViewListener.GetListenPort() )
	{
		ViewSession* pNewUser = gViewContainer.CreateUser(pIncomingConn, m_TimerAxis);

		// 如果网关未与管理服握手完毕，不接受连接
		if( !m_MngConnector.IsReady())
		{
			ErrorLn(_GT("m_MngConnector is not ready! So I can't accept ViewServer!"));
			pNewUser->Release();
			return ;
		}

		// 是否超过最大连接数
		if (gViewContainer.IsFull())
		{
			ErrorLn(_GT("View Sessions is Full!"));
			pNewUser->Release();
			return;
		}

		*ppHandler = pNewUser;

		char szName[64] = {0};
		sprintf_s(szName, sizeof(szName), "ViewServer通道监听到新连接 Pointer[%d]",pNewUser);
		pIncomingConn->setName(szName);
	}
	else
	{
		assert(false);
	}
}



////////////////////////// EventHandler ///////////////////////////
void GlobalViewGateway::OnEvent( HANDLE event )
{
	if ( event==m_hTimer )
	{
		m_TimerAxis.CheckTimer();

		// 执行操作代码
		if (m_dwControlCode>VS_ControlCode_None && m_dwControlCode<VS_ControlCode_Max)
		{
			DoControlCodeTask(m_dwControlCode);
			m_dwControlCode = VS_ControlCode_None;
		}
	}
	else
	{
		DispatchNetworkEvents();
	}
}



/**
@purpose          : 定时器触发后回调,你可以在这里编写处理代码
@param	 dwTimerID: 定时器ID,用于区分是哪个定时器
@return		      : empty
*/
void GlobalViewGateway::OnTimer(unsigned long dwTimerID)
{
	switch(dwTimerID)
	{
	case ETimerEventID_Keepalive:			//  检查心跳
		{
			//DWORD dwNowTimes = (DWORD)time(NULL);	// 时间
			//DWORD dwNowTicks = ::GetTickCount();	// Tick时间

		}
		break;

	case ETimerEventID_VoiceControl:
		{
			if( !m_bUpdate && m_bNewDataCome )
			{
				//ErrorLn("可以执行了，先加上锁");
				m_bUpdate = true;
				
				ExcutCommand( m_nNum1, m_nNum2, m_strTxt.c_str() );

				m_bNewDataCome = false;
				m_bUpdate = false;
				//ErrorLn("执行完了，解锁锁");
			}
		}
		break;

	case ETimerEventID_VoiceNetCount:
		{
			m_TotalInfo.dwRecvSpeedServer = m_dwRecvSpeedServer;
			m_TotalInfo.dwSendSpeedServer = m_dwSendSpeedServer;
			m_TotalInfo.dwRecvSpeedClient = m_dwRecvSpeedClient;
			m_TotalInfo.dwSendSpeedClient = m_dwSendSpeedClient;
			
			m_dwRecvSpeedServer = 0;
			m_dwSendSpeedServer = 0;
			m_dwRecvSpeedClient = 0;
			m_dwSendSpeedClient = 0;

		}	
		break;

	default:break;
	}
	
}


/** 启用定时器
@param   
@param   
@return  
*/ 
bool GlobalViewGateway::SetTimer(DWORD timerID, DWORD interval, ITimerHandler * handler, LPCSTR debugInfo, DWORD callTimes)
{
	TimerAxis * pTimerAxis = getTimerAxis();
	if(pTimerAxis == NULL)
	{
		return false;
	}

	return pTimerAxis->SetTimer(timerID, interval, handler, callTimes, debugInfo);
}

/** 销毁定时器
@param   
@param   
@return  
*/
bool GlobalViewGateway::KillTimer(DWORD timerID, ITimerHandler * handler)
{
	TimerAxis * pTimerAxis = getTimerAxis();
	if(pTimerAxis == NULL)
	{
		return false;
	}

	return pTimerAxis->KillTimer(timerID, handler);
}


/// purpose: 服务器网络消息处理
void GlobalViewGateway::onServerMessage( ulong actionId, SGameMsgHead* head, void* data, size_t len)
{
	switch(actionId)
	{
	case MSG_VOICE_ANSWER_KEEPALIVE:		// 心跳检查
		{
			m_MngConnector.m_bIsAnswer = true;							// 是否有回应
			m_MngConnector.m_dwLastAnswerTime = ::GetTickCount();		// 最后收到心跳Tick时间
			//TraceLn("收到心跳回应!");
		}
		break;
	case MSG_VOICE_HANDSHAKE_RESPONSE:		// 回应握手消息
		{
			OnMsgServerHandshakeResponse(actionId,head,data, len);
		}
		break;

	case MSG_VOICE_SENDDATA:		// 请求语音网关服务发送数据给各客户端消息
		{
//			OnMsgServerSendData(actionId,head,data, len);
		}
		break;

	case MSG_VOICE_DATA_SUBMSG:			// 语音网关子消息
		{
//			OnMsgServerSubMsg( actionId,head,data, len );
		}
		break;

	case MSG_VOICE_PERFORMANCE:		// 请求性能检测消息包
		{
//			OnMsgServerPerformance(actionId,head,data, len);
		}
		break;

	default:
		{
			ErrorLn(_GT("尚有一个语音服务器消息未处理，actionId = ")<<actionId);
		}
		break;
	}
}



//////////////////////////////////////////////////////////////////////////
/// purpose: 客户端连入
void GlobalViewGateway::OnClientUserEnter(CClientUser *pUser, DWORD dwReason )
{
	if( NULL == pUser )
	{
		return ;
	}

}

/// purpose: 客户端断线
void GlobalViewGateway::OnClientUserLeave(CClientUser *pUser, DWORD dwReason )
{
	if (NULL==pUser)
	{
		return;
	}

}


// 命令操作
void GlobalViewGateway::ExcutCommand( int nNum1, int nNum2, const char * szTxt )
{
	switch(nNum1)
	{
	case VoiceCommand_ExportUserList:		// 导出用户列表
		//CClientList::getInstance().SaveUserListToCSV();
		break;

	case VoiceCommand_ChangeMaxConnection:	// 修改最大连接数
		{
			if( nNum2 < 0 )
			{
				nNum2 = 0;
			}

			gSetting.m_dwMaxConnection = nNum2;

			ErrorLn( _GT("修改最大连接数为：") << gSetting.m_dwMaxConnection);
		}
		break;
	}
}



/// purpose: 预留
void GlobalViewGateway::OnMsgClientSendData(CClientUser &client, ulong actionId, SGameMsgHead* head, void* data, size_t len)
{
	//	if(data==NULL|| len <= sizeof(SMsgVoiceSendData))
	//	{
	//		return;
	//	}
	//
	//	SMsgVoiceSendData * pMsg = (SMsgVoiceSendData *)(data);
	//
	//#ifdef VOICE_PRINT_DEBUG_INF
	//	// 调式代码
	//	char szBuf[512]={0};
	//	sprintf_s(szBuf, _countof(szBuf),_NGT"GlobalViewGateway::OnMsgClientSendData() 发送数据给各软件客户端消息:软件客户端ID=%d,数据长度=%d",
	//		pMsg->dwServerID,pMsg->nDataLens);
	//	//TraceLn(szBuf);
	//#endif
	//
	//	DWORD dwSendDataLens = (DWORD)(len-sizeof(SMsgVoiceSendData) );
	//
	//	if (dwSendDataLens!=pMsg->nDataLens)
	//	{
	//		ErrorLn("请求桥客户端网关服务发送数据给各软件客户端消息长度错误! 实际长度="<<dwSendDataLens<<",指示长度="<<pMsg->nDataLens);
	//	}
	//
	//	// 转发数据,为了提高效率,用修改头数据方法,必须结构体大小一样
	//	Assert(sizeof(SMsgVoiceSendData)==sizeof(SMsgVoiceHead));
	//
	//	// 保留转发参数,下面的代码不要用 pMsg了,已被覆盖了
	//	SMsgVoiceSendData msgHead = (*pMsg);
	//
	//	head->SrcEndPoint = MSG_ENDPOINT_VOICE;
	//	head->DestEndPoint= MSG_ENDPOINT_VOICE;
	//	head->byKeyModule  = MSG_MODULEID_VOICE;
	//	head->byKeyAction  = MSG_VOICE_SENDDATA;
	//
	//	// 通过跨区桥客户端中转消息必须加入此消息体
	//	SMsgVoiceHead * pBhead =(SMsgVoiceHead *)data;
	//	pBhead->dwWorldID	= client.m_dwWorldID;		// 游戏世界ID
	//	pBhead->dwServerID	= client.GetServerID();		// 客户端ID
	//
	//	LPSTR pSendData = (LPSTR)head;
	//	dwSendDataLens = (DWORD)(len+sizeof(SGameMsgHead));
	//
	//	if (msgHead.dwServerID==0)
	//	{
	//		// 广播数据给客户端 pServerArray==NULL && wServerNum==VOICE_BROADCASTALL_NUM 广播所有
	//		BroadcastToServer(NULL,VOICE_BROADCASTALL_NUM,pSendData,dwSendDataLens);
	//	} 
	//	else
	//	{
	//		// 发送数据给指定客户端
	//		SendDataToServer(msgHead.dwServerID,pSendData,dwSendDataLens);
	//	}

}

/// purpose: 预留
void GlobalViewGateway::OnMsgClientBroadcastData(CClientUser &client, ulong actionId, SGameMsgHead* head, void* data, size_t len)
{

	//	if(data==NULL|| len <= sizeof(SMsgVoiceBroadcastData))
	//	{
	//		return;
	//	}
	//
	//	SMsgVoiceBroadcastData * pMsg = (SMsgVoiceBroadcastData *)(data);
	//
	//#ifdef VOICE_PRINT_DEBUG_INF
	//	// 调式代码
	//	char szBuf[512]={0};
	//	sprintf_s(szBuf, _countof(szBuf),_NGT"GlobalViewGateway::OnMsgClientBroadcastData() 广播数据给各软件客户端消息:软件客户端ID列表个数=%d,数据长度=%d",
	//		pMsg->dwServerCounts,pMsg->nDataLens);
	//	//TraceLn(szBuf);
	//#endif
	//
	//	if (pMsg->dwServerCounts<=0)
	//	{
	//		ErrorLn("请求桥客户端网关服务广播数据给各软件客户端消息时要广播客户端个数为0");
	//		return;
	//	}
	//
	//	DWORD dwServerNum = pMsg->dwServerCounts;
	//	DWORD * pServerArray = (DWORD *)((LPSTR)data + sizeof(SMsgVoiceBroadcastData));
	//	DWORD dwSendDataLens = (DWORD)(len-sizeof(SMsgVoiceBroadcastData)-dwServerNum*sizeof(DWORD));
	//
	//	if (dwSendDataLens!=pMsg->nDataLens)
	//	{
	//		ErrorLn("请求桥客户端网关服务广播数据给各软件客户端消息长度错误! 实际长度="<<dwSendDataLens<<",指示长度="<<pMsg->nDataLens);
	//	}
	//
	//	LPSTR pSendData = (LPSTR)((LPSTR)data + sizeof(SMsgVoiceBroadcastData)+dwServerNum*sizeof(DWORD));
	//
	//
	//	// 转发数据,为了提高效率,用修改头数据方法,必须结构体大小一样
	//	Assert(sizeof(SMsgVoiceBroadcastData)>=sizeof(SMsgVoiceHead));
	//
	//	// 拷贝列表,否则会被中转消息头覆盖
	//	DWORD dwServerList[1024];
	//	memcpy(dwServerList,pServerArray,dwServerNum*sizeof(DWORD));
	//
	//
	//	SGameMsgHead* pHead = (SGameMsgHead*)(pSendData - sizeof(SGameMsgHead)- sizeof(SMsgVoiceHead));
	//
	//	pHead->SrcEndPoint = MSG_ENDPOINT_VOICE;
	//	pHead->DestEndPoint= MSG_ENDPOINT_VOICE;
	//	pHead->byKeyModule  = MSG_MODULEID_VOICE;
	//	pHead->byKeyAction  = MSG_VOICE_SENDDATA;
	//
	//	// 通过跨区桥客户端中转消息必须加入此消息体
	//	SMsgVoiceHead * pBhead =(SMsgVoiceHead*)(pSendData - sizeof(SMsgVoiceHead));
	//	pBhead->dwWorldID	= client.m_dwWorldID;		// 游戏世界ID
	//	pBhead->dwServerID	= client.GetServerID();		// 客户端ID
	//
	//	pSendData = (LPSTR)pHead;
	//	dwSendDataLens = (DWORD)(dwSendDataLens+sizeof(SGameMsgHead)+sizeof(SMsgVoiceHead));
	//
	//	// 广播数据给客户端 pServerArray==NULL && wServerNum==VOICE_BROADCASTALL_NUM 广播所有
	//	BroadcastToServer(dwServerList,(WORD)dwServerNum,pSendData,dwSendDataLens);

}


//////////////////////////////////////////////////////////////////////////

/// purpose: 语音服务器发来握手回应消息
void GlobalViewGateway::OnMsgServerHandshakeResponse(ulong actionId, SGameMsgHead* head, void* data, size_t len)
{
	if(data==NULL|| len != sizeof(SMsgVoiceHandshakeResponse_VG))
	{
		return;
	}

	SMsgVoiceHandshakeResponse_VG * pMsg = (SMsgVoiceHandshakeResponse_VG *)(data);

#ifdef VOICE_PRINT_DEBUG_INF
	// 调式代码
	char szBuf[512]={0};
	sprintf_s(szBuf,sizeof(szBuf), "GlobalViewGateway::OnMsgServerHandshake() 服务器握手消息:ID=%d",pMsg->dwID);
	TraceLn(szBuf);
#endif

	m_MngConnector.SetID(pMsg->dwID);

	m_TotalInfo.dwGatewayID = pMsg->dwID;

	//m_bReady = true;

	TraceLn(_GT("收到语音服务器回应握手信息! ") << a2utf8(m_MngConnector.ToString().data()));
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////			公共方法					///////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::string GetLastErrorString(DWORD dwErrorCode)
{
	void* lpMsgBuf = NULL;
	FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		dwErrorCode,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(char*) &lpMsgBuf,  // LPTSTR
		0, NULL );
	static std::string strErr;
	if ( NULL == lpMsgBuf )
	{
		string strErrCode;
		strErr = "未知错误!错误码为" + StringHelper::fromUlong(strErrCode, dwErrorCode);
	}
	else
	{
		strErr = (char*)lpMsgBuf;
		LocalFree(lpMsgBuf);
	}

	return strErr;
}


/** 取得字节大小显示字串
@param  ULONGLONG dwSize:字节大小
@return  LPCSTR
*/	
LPCSTR GetFileSizeString(ULONGLONG dwSize)
{
	static char szBuf[64] = {0};

	double fSize = (double)dwSize;

	memset(szBuf,0,sizeof(szBuf));

	if(dwSize<1024)
	{
		sprintf_s(szBuf, sizeof(szBuf), "%I64d 字节",dwSize);
	}
	else if(dwSize<1048576)
	{
		fSize = fSize/1024.0;
		sprintf_s(szBuf, sizeof(szBuf), "%.1f KB",fSize);
	}
	else if (dwSize<1073741824)
	{
		fSize = fSize/1048576.0;
		sprintf_s(szBuf, sizeof(szBuf), "%.1f MB",fSize);
	}
	else
	{
		fSize = fSize/1073741824.0;
		sprintf_s(szBuf, sizeof(szBuf), "%.1f GB",fSize);
	}
	return szBuf;
}

/** 取得剩余时间字串
@param  char * szBuf:字串buffer
@param  int nBufSize:字串buffer长度
@param  DWORD nRemainSeconds:剩余时间(秒)
@return  
*/	
void GetRemainTimeString(char * szBuf,int nBufSize,DWORD nRemainSeconds)
{
	// 剩余时间计数
	int bNums=0,sNums=0;

	// 小于等于60秒
	if (nRemainSeconds<60)
	{
		sprintf_s(szBuf, nBufSize, "%d秒",nRemainSeconds);
	}
	else if(nRemainSeconds<3600)
	{// 小于1小时
		bNums=(int)(nRemainSeconds / 60) ;
		sNums =(int)(nRemainSeconds % 60) ;
		if (sNums==0)
		{
			sprintf_s(szBuf, nBufSize, "%d分钟",bNums );
		} 
		else
		{
			sprintf_s(szBuf, nBufSize, "%d分钟%d秒",bNums,sNums);
		}

	}
	else if (nRemainSeconds<86400)
	{ // 小于1天
		bNums=(int)(nRemainSeconds / 3600) ;
		sNums =(int)((nRemainSeconds % 3600)/60) ;	
		if (sNums==0)
		{
			sprintf_s(szBuf, nBufSize, "%d小时",bNums );
		} 
		else
		{
			sprintf_s(szBuf, nBufSize, "%d小时%d分钟",bNums,sNums);
		}
	} 
	else
	{//大于等于1天
		bNums=(int)(nRemainSeconds / 86400) ;
		sNums =(int)((nRemainSeconds % 86400)/3600) ;
		if (sNums==0)
		{
			sprintf_s(szBuf, nBufSize, "%d天",bNums );
		} 
		else
		{
			sprintf_s(szBuf, nBufSize, "%d天%d小时",bNums,sNums);
		}
	}
}

/** 取得剩余时间字串
@param  DWORD nRemainSeconds:剩余时间(秒)
@return  LPCSTR
*/	
LPCSTR GetRemainTimeString(DWORD nRemainSeconds)
{
	static char szTimebuf[10][32] = {0};
	static BYTE nBufIndex = 0;

	nBufIndex++;
	if (nBufIndex>=10)
	{
		nBufIndex =0;
	}

	memset(szTimebuf[nBufIndex],0,sizeof(szTimebuf[nBufIndex]));
	GetRemainTimeString(szTimebuf[nBufIndex],sizeof(szTimebuf[nBufIndex]),nRemainSeconds);
	return szTimebuf[nBufIndex];
}

/** 取得时间字串
@param   DWORD dwTime:时间
@param   
@return  LPCSTR
*/
LPCSTR GetTimeString(DWORD dwTime)
{
	static char szTimebuf[10][32] = {0};
	static BYTE nBufIndex = 0;

	nBufIndex++;
	if (nBufIndex>=10)
	{
		nBufIndex =0;
	}

	memset(szTimebuf[nBufIndex],0,sizeof(szTimebuf[nBufIndex]));
	if (dwTime==0)
	{
		return szTimebuf[nBufIndex];
	}
	time_t t = (time_t) dwTime;
	tm local;
	memset(&local,0,sizeof(local));
	localtime_s(&local,&t);

	sprintf_s(szTimebuf[nBufIndex], sizeof(szTimebuf[nBufIndex]), "%d-%.2d-%.2d %.2d:%.2d:%.2d",
		1900+local.tm_year,local.tm_mon+1,local.tm_mday,local.tm_hour,local.tm_min,local.tm_sec);

	return szTimebuf[nBufIndex];
}

// 取得服务器类型名称
_TCHAR * GetServerTypeString(DWORD dwServerType)
{
	//游戏软件服务器
	static _TCHAR *_gszGameSoftServerName[MSG_SERVERID_MAXID] =
	{
		_T("未知游戏服务器"),        // 未定义	
		_T("场景服务器"),            // 场景服务器	    ［简称：S］
		_T("网关服务器"),            // 网关服务器	    ［简称：G］
		_T("登录服务器"),            // 登录服务器	    ［简称：L］
		_T("世界服务器"),            // 世界服务器	    ［简称：W］
		_T("游戏客户端"),            // 客户端			［简称：C］
		_T("中心服务器"),            // 中心服务器	    ［简称：E］
		_T("验证码服务器"),          // 验证服务器	    ［简称：Y］
        _T("社会服务器"),            // 社会服务器	    ［简称：O］
		_T("跨区桥服务器"),          // 跨区桥服务器	［简称：B］
		_T("DB应用服务器"),          // DB应用服务器		［简称：D］
		_T("通行证服务器"),          // 通行证服务器		［简称：P］
		_T("数据监护服务器"),        // 数据监护服务器	［简称：G］
		_T("语音服务器"),            // 语音服务器	［简称：V］	
		_T("语音网关"),              // 语音网关		［简称：VG］
	};
	if (dwServerType<MSG_SERVERID_MAXID)
	{
		return _gszGameSoftServerName[dwServerType];
	}
	return _gszGameSoftServerName[0];
}