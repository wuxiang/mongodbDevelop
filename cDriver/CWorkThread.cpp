#include "CWorkThread.h"
#include "g_function.h"

CWorkThread::CWorkThread( void )
{
}

CWorkThread::~CWorkThread( void )
{
}

int  CWorkThread::getInformFd()
{
	return m_fdPipe[1];
}

bool CWorkThread::addResToWorkThread(Actor& ptr)
{
	GW_lock_guard   lock(m_mtx4Sve);
	m_slaveListPtr->push_back(ptr);

	return true;
}

std::list<Actor>*  CWorkThread::getAndResetSlaveListPtr()
{
	std::list<Actor>* ptr = NULL;
	{
		GW_lock_guard   lock(m_mtx4Sve);
		if (m_slaveListPtr->size())
		{
			ptr =  m_slaveListPtr;
			m_slaveListPtr = new std::list<Actor>();
		}
	}
	return ptr;
}

bool CWorkThread::Start()
{
	pthread_t thrd;
	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setstacksize(&attr, 1024 * 1024 * 2 );

	if(pthread_create( &thrd, &attr, CWorkThread::ThreadProc, (CWorkThread*)this))
	{
		g_log.Wlog( 0, "CWorkThread created error...\n" );
		return false;
	}
	else
	{
		g_log.Wlog( 0, "CWorkThread created =%lu\n", thrd );
		pthread_detach( thrd );
	}

	return true;;
}

int  CWorkThread::getTotalConNum()
{
	return m_TotalConn;
}

void* CWorkThread::ThreadProc( void* param )
{
	CWorkThread *pThread = ( CWorkThread * )param;

	if( pThread && pThread->InitInstance() )
	{
		pThread->ThreadWork();
		pThread->ExitInstance();
	}

	return (void*)(0);
}

bool CWorkThread::InitInstance()
{
	// init mongodb
	m_mongoSelf.setMongoInfo(Config::instance().get_DB_IP(),
							 Config::instance().get_DB_port(),
							 NULL,
							 NULL);
	m_mongoCloud.setMongoInfo(Config::instance().get_cloud_ip(),
							  Config::instance().get_cloud_port(),
							  NULL,
							  NULL);
	if (!m_mongoSelf.mongoConnect() || !m_mongoCloud.mongoConnect())
	{
		g_log.Wlog(0, "connect to mongodb failed when start CWorkThread!!!");
		exit(-1);
	}

	/***********************init connection information**********************/
	m_PreCheckTime = time( 0 );
	m_TotalConn = 0 ;
	m_epfd = epoll_create( Config::instance().getConPerThread());
	m_Bind = new BindPtr[Config::instance().getConPerThread()];

	if( m_Bind )
	{
		for( int i = 0; i < Config::instance().getConPerThread(); ++i)
		{
			m_Bind[i].fd  = -1;
			m_Bind[i].addr.sin_addr.s_addr = 0;
			m_Bind[i].bClose = 0;
			m_Bind[i].bLogin = 0;
			m_Bind[i].LastAlive = m_PreCheckTime ;
			m_Bind[i].RBuf.Reserve();
			m_Bind[i].SBuf.Reserve();
		}
	}
	else
	{
		g_log.Wlog( 0, "CWorkThread::InitInstance m_Bind malloc ERR\n" );
		return false;
	}

	sockaddr_in clientaddr;
	memset( &clientaddr, 0, sizeof( sockaddr_in ) );
	pipe(m_fdPipe );
	CSockOpt::SetNonBlocking(m_fdPipe[0] );
	CSockOpt::SetNonBlocking(m_fdPipe[1] );
	AddClient(m_fdPipe[0], &clientaddr );

	return true;
}

void CWorkThread::ExitInstance()
{
	return ;
}

void CWorkThread::processThreadMsg()
{
	g_log.Wlog(3, "CWorkThread::processThreadMsg handle new connection\n");
	pTHREADDATA pHD = ( pTHREADDATA )( m_pCurr->RBuf.Data() );

	size_t len = 0;
	size_t sizeOfThreadData = sizeof(THREADDATA);
	while( m_pCurr->RBuf.Size() - len >=  sizeOfThreadData)
	{
		if (AddClient( pHD->fd, &pHD->addr ))
		{
			close(pHD->fd);
		}

		pHD++;
		len += sizeOfThreadData;
	}

	m_pCurr->RBuf.CutHead( len );
}

int CWorkThread::AddClient( int fd, sockaddr_in *addr )
{
	g_log.Wlog( 1, "AddClient fd = %d(%s:%d)\n", fd, inet_ntoa( addr->sin_addr ), addr->sin_port );

	struct epoll_event ev;
	for ( int i = 0; i < Config::instance().getConPerThread(); i++ )
	{
		if( m_Bind[i].fd < 0 )
		{
			m_Bind[i].fd = fd;
			memcpy( &m_Bind[i].addr, addr, sizeof( sockaddr_in ) );
			m_Bind[i].bLogin = 0 ;
			m_Bind[i].bClose = 0 ;
			m_Bind[i].LastAlive = time( 0 );
			m_Bind[i].RBuf.Reserve(5120);
			m_Bind[i].SBuf.Reserve(5120);

			ev.data.ptr = ( void* )&m_Bind[i];
			ev.events = EPOLLIN | EPOLLET;
			if( !epoll_ctl( m_epfd, EPOLL_CTL_ADD, fd, &ev ) )
			{
				++m_TotalConn;
				return 0;
			}
		}
	}

	return -1;
}

void CWorkThread::CloseClient( BindPtr *pCurr, const char* pszErr )
{
	if( pCurr->fd >= 0 )
	{
		struct epoll_event ev;
		g_log.Wlog( 1, "CloseClient close fd=%d[%s]\n", pCurr->fd , pszErr );
		epoll_ctl( m_epfd, EPOLL_CTL_DEL, pCurr->fd, &ev );
		close( pCurr->fd );		//其实,关闭句柄会自动清除epoll中的,保险还是自己先删除
		pCurr->fd = -1;
		--m_TotalConn;
	}

	return;
}

void  CWorkThread::eventHandle(struct epoll_event* events, int nfds, time_t& CurrTime)
{
	int rt = 0;
	int tmRet = 0;

	struct epoll_event ev;

	for(int i = 0; i < nfds; ++i )
	{
		rt = 0;
		m_pCurr = (BindPtr*)( events[i].data.ptr );

		if( !m_pCurr || ( m_pCurr->fd < 0 ) )
		{
			continue;
		}

		if( events[i].events & EPOLLIN )   //可以接收数据事件
		{
			rt = RecvData( m_pCurr->fd, &m_pCurr->RBuf);
			m_pCurr->LastAlive = CurrTime ;
			g_log.Wlog( 5, "threadworking RecvData and buffer information  fd=%d, rt=%d(SPos=%d, EPos=%d)\n", m_pCurr->fd, rt , m_pCurr->RBuf.get_spos(), m_pCurr->RBuf.get_epos());
		}

		if( events[i].events & EPOLLOUT )   //可以发送事件
		{
			rt = SendData( m_pCurr->fd, &m_pCurr->SBuf );
			g_log.Wlog( 5, "threadworking SendData and buffer information fd=%d, rt=%d(SPos=%d, EPos=%d)\n", m_pCurr->fd, rt , m_pCurr->SBuf.get_spos(), m_pCurr->SBuf.get_epos());
		}

		if( rt < 0 )
		{
			CloseClient( m_pCurr, "recv/send ERR." );
		}
		else if( m_pCurr->bClose && ( m_pCurr->SBuf.Size() == 0 ) )
		{
			CloseClient( m_pCurr, "recv/send END." );
		}
		else
		{
			ev.data.ptr = m_pCurr;
			ev.events = EPOLLIN | EPOLLET;

			if( m_pCurr->SBuf.Size() > 0 )
			{
				ev.events |= EPOLLOUT ; //如果没有发完,继续投递
			}

			tmRet = epoll_ctl( m_epfd, EPOLL_CTL_MOD, m_pCurr->fd, &ev);
		}
	}
}

void CWorkThread::CheckTimeOut()
{
	for( int i = 1; i < Config::instance().getConPerThread(); i++ )
	{
		if( ( m_Bind[i].fd >= 0 ) && ( m_CurrTime - m_Bind[i].LastAlive >= Config::instance().getTimeout()))
		{
			CloseClient( &m_Bind[i], "timeOUT" );
		}
	}

	return;
}

void CWorkThread::ThreadWork()
{
	g_log.Wlog( 0, "ThreadWorking ThreadID=%lu\n", pthread_self() );

	int nfds;
	struct epoll_event events[1024];

	while( 1 )
	{
		//处理远程返回数据
		processAccCenterReturn();

		nfds = epoll_wait( m_epfd, events, 1024, 20 );
		m_CurrTime = time(0);
		if (nfds > 0)
		{
			eventHandle( events, nfds, m_CurrTime);
		}

		//超时处理
		if( m_CurrTime - m_PreCheckTime >= Config::instance().getTimeout() / 5 )
		{
			CheckTimeOut();
			m_PreCheckTime = m_CurrTime ;
		}
	}
}
/*
返回值:
>=0,实际接收到的数据长度
-1，断开
-2，数据错误
-3，数据太长,缓冲区已满
buf:
|---------------------------------------MaxLen---------------------------------------|
 ↑StartPos      ↑EndPos
*/
int CWorkThread::RecvData( int fd, pSBuffer Rbuf )
{
	if( Rbuf->EPos == SBUF_SIZE )
	{
		if( Rbuf->SPos > 0 )
		{
			memmove( Rbuf->Buf, Rbuf->Buf + Rbuf->SPos, Rbuf->EPos - Rbuf->SPos );
			Rbuf->EPos -= Rbuf->SPos;
			Rbuf->SPos = 0;
		}
		else
		{
			return -3;
		}
	}

	int length = read( fd , Rbuf->Buf + Rbuf->EPos , SBUF_SIZE - Rbuf->EPos );
	g_log.Wlog( 5, "ThreadWroking recv fd=%d,length=%d\n", fd, length );

	if( length == 0 ) //断开
	{
		return -1;
	}

	if( length < 0 )
	{
		if( errno == EAGAIN  ||  errno == EINTR )
		{
			return 0;
		}
		else
		{
			return -1;
		}
	}

	Rbuf->EPos += length;

	if( fd == m_fdPipe[0] )		//处理线程消息
	{
		processThreadMsg();
		return length;
	}

	if( Rbuf->EPos - Rbuf->SPos >= 1 )
	{
		processPacket();
	}

	return length;
}

int CWorkThread::RecvData( int fd, CDBuffer *Rbuf )
{
	if( Rbuf->Space() == 0 )
	{
		Rbuf->ExpBuf();

		if( Rbuf->Space() == 0 )
		{
			return -3;
		}
	}

	int length = 0;
	length = read( fd , Rbuf->Curr() , Rbuf->Space() );
	g_log.Wlog( 5, "ThreadWroking recv fd = %d,length = %d\n", fd, length );

	if( length == 0 ) //断开
	{
		return -1;
	}

	if( length < 0 )
	{
		if( errno == EAGAIN  ||  errno == EINTR )
		{
			return 0;
		}
		else
		{
			return -1;
		}
	}

	Rbuf->Lseek( length );
	g_log.Whex( 5, Rbuf->Data(), Rbuf->Size() );

	if( fd == m_fdPipe[0] )		//处理线程消息
	{
		processThreadMsg();
		return length;
	}

	if( Rbuf->Size() >= 1 )
	{
		processPacket();
	}

	return length;
}

int CWorkThread::SendData( int fd, pSBuffer Sbuf )
{
	int ret = 0;
	int DataLen = Sbuf->EPos - Sbuf->SPos ;

	if( DataLen > 0 )
	{
		ret = send( fd , Sbuf->Buf + Sbuf->SPos , DataLen , 0 );

		if( ret < 0 )
		{
			if( ( errno == EAGAIN ) || ( errno == EINTR ) )
			{
				return 0;
			}
			else
			{
				return -1;
			}
		}
		else if( ret == 0 )
		{
			return -1;
		}
		else
		{
			if( ret < DataLen )
			{
				Sbuf->SPos += ret;
			}
			else
			{
				Sbuf->SPos = Sbuf->EPos = 0;
			}
		}
	}

	return ret;
}
int CWorkThread::SendData( int fd, CDBuffer *Sbuf )
{
	ssize_t ret = 0;

	if( Sbuf->Size() > 0 )
	{
		ret = send( fd , Sbuf->Data() , Sbuf->Size() , 0 );

		if( ret < 0 )
		{
			if( ( errno == EAGAIN ) || ( errno == EINTR ) )
			{
				return 0;
			}
			else
			{
				return -1;
			}
		}
		else if( ret == 0 )
		{
			return -1;
		}
		else
		{
			if( (size_t)ret < Sbuf->Size() )
			{
				Sbuf->CutHead( ret );
			}
			else
			{
				Sbuf->Clear();
			}
		}
	}

	return ret;
}


int CWorkThread::SendData( int fd, pSIovec SIov )
{
	uint32_t size = -1;
	int nIov = SIov->EndPos - SIov->StartPos ;
	size = writev( fd, SIov->iov + SIov->StartPos , nIov );

	if( SIov->TotalLen == size )
	{
		memset( SIov, 0, sizeof( SIovec ) );
		return size;
	}

	if( size < 0 )
	{
		if( ( errno == EAGAIN ) || ( errno == EINTR ))
		{
			return 0;
		}
		else
		{
			return -1;
		}
	}
	else if( size == 0 )
	{
		return -1;
	}
	else
	{
		//将后面的数据指向前面的指针上
		size_t nPre = 0;

		for( size_t i = SIov->StartPos; i < SIov->EndPos; i++ )
		{
			nPre += SIov->iov[i].iov_len;

			if( size == nPre )
			{
				//后面的移动指向
				size_t nMovePos = i + 1;

				size_t j;
				for( j = 0; j < SIov->EndPos - nMovePos ; ++j)
				{
					SIov->iov[j].iov_base = SIov->iov[j + nMovePos].iov_base;
					SIov->iov[j].iov_len = SIov->iov[j + nMovePos].iov_len;
				}

				SIov->TotalLen -= size;
				SIov->StartPos = 0;
				SIov->EndPos = j;
				return size;
			}

			if( size < nPre )
			{
				//后面的移动指向,还要当前的移动一半
				SIov->iov[0].iov_base = ( char* )SIov->iov[i].iov_base + ( nPre - size );
				SIov->iov[0].iov_len = SIov->iov[i].iov_len - ( nPre - size );
				size_t j;

				for( j = 1; j < SIov->EndPos - i ; j++ )
				{
					SIov->iov[j].iov_base = SIov->iov[j + i].iov_base;
					SIov->iov[j].iov_len = SIov->iov[j + i].iov_len;
				}

				SIov->TotalLen -= size;
				SIov->StartPos = 0;
				SIov->EndPos = j ;
				return size;
			}
		}
	}

	return size;
}

bool  CWorkThread::queryAllStockFromDB(const std::string& name, std::vector<AtomRecordDB>& vRes)
{
	bson query;
	bson_init(&query);
	{
		bson_append_string(&query, "UName", name.c_str());
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_int(&field, "sort", 1);
		bson_append_int(&field, "group", 1);
		bson_append_int(&field, "time", 1);
		bson_append_int(&field, "content", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	m_mongoSelf.getCursor(Config::instance().getStockDataDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	time_t   tm = 0;;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		AtomRecordDB  tmpRec;
		tmpRec.uname = name;

		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "sort"))
		{
			tmpRec.sort = (char)bson_iterator_int(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "group"))
		{
			tmpRec.group = (char)bson_iterator_int(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			tmpRec.ver = bson_iterator_time_t(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson_iterator subIter[1];
			bson_iterator_subiterator(it, subIter);

			// scanner array object
			while (BSON_EOO != bson_iterator_next(subIter))
			{
				bson obj[1];
				bson_iterator_subobject(subIter, obj);

				bson_iterator  objIte[1];
				if (BSON_EOO != bson_find(objIte, obj, "data"))
				{
					stk = bson_iterator_string(objIte);

					tm = 0;
					if (BSON_EOO != bson_find(objIte, obj, "time"))
					{
						tm = bson_iterator_time_t(objIte);
					}

					tmpRec.content.push_back(_SymbolStock(stk, tm));
				}
			}
		}

		vRes.push_back(tmpRec);
		flag = true;
	}

	if (!flag)
	{
		//TODO
		//check error flag for mongodb
	}

	mongo_cursor_destroy(&cursor);
	bson_destroy(&field);
	bson_destroy(&query);

	return flag;
}

bool  CWorkThread::queryAllStockFromCloudDB(const std::string& name, std::vector<AtomRecordDB>& vRes)
{
	bson query;
	bson_init(&query);
	{
		bson_append_string(&query, "userid", name.c_str());
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_int(&field, "time", 1);
		bson_append_int(&field, "content.group0", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	m_mongoCloud.getCursor(Config::instance().getSyncronizeDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	time_t tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		AtomRecordDB  tmpRec;
		tmpRec.uname = name;
		tmpRec.sort = 1;
		tmpRec.group = 1;

		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			tmpRec.ver = bson_iterator_time_t(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson  subobj[1];
			bson_iterator_subobject(it, subobj);

			bson_iterator  subIter[1];
			if (BSON_EOO != bson_find(subIter, subobj, "group0"))
			{
				bson_iterator  subsubIter[1];
				bson_iterator_subiterator(subIter, subsubIter);
				while (MONGO_OK == bson_iterator_next(subsubIter))
				{
					bson  desObj[1];
					bson_iterator_subobject(subsubIter, desObj);

					bson_iterator  stkIte[1];
					if (BSON_EOO != bson_find(stkIte, desObj, "data"))
					{
						stk = bson_iterator_string(stkIte);
						tm = 0;
						if (BSON_EOO != bson_find(stkIte, desObj, "time"))
						{
							tm = bson_iterator_time_t(stkIte);
						}

						tmpRec.content.push_back(_SymbolStock(stk, tm));
					}
				}
			}
		}

		vRes.push_back(tmpRec);
		flag = true;
	}

	if (!flag)
	{
		//TODO
	}
	mongo_cursor_destroy(&cursor);
	bson_destroy(&field);
	bson_destroy(&query);

	return flag;
}

bool CWorkThread::queryAllStockData4User(const std::string& name, std::vector<AtomRecordDB>& vRes)
{
	bool flag = queryAllStockFromCloudDB(name, vRes);
	flag = flag && queryAllStockFromCloudDB(name, vRes);
	return flag;
}

bool CWorkThread::Cmd_Download(FoundBase* pCmd, BindPtr* pBind)
{
	g_log.Wlog(3, "%s: %d starting!!!\n", __FUNCTION__, __LINE__);
	if (!pCmd || !pBind)
	{
		return false;
	}

	FoundDownload*  pDownload = dynamic_cast<FoundDownload*>(pCmd);
	if (!pDownload)
	{
		return false;
	}

	CDBuffer  rBuf;
	char  rRetNum = 0;
	char rRet = 0;

	char errorStr[1024] = { 0 };

	if (pDownload->rCH.sort == 0 && pDownload->rCH.group == 0)
	{
		short shortlen = 0;
		std::vector<AtomRecordDB>  vRes;
		queryAllStockData4User(pDownload->name, vRes);
		std::vector<AtomRecordDB>::iterator iter = vRes.begin();
		while (iter != vRes.end())
		{
			char st = (char)(iter->sort);
			char gp = (char)(iter->group);
			rBuf.appendChar(st);
			rBuf.appendChar(gp);
			rBuf.appendInt64(iter->ver);
			rBuf.appendShort(shortlen);
			if (1 == st && 1 == gp)
			{
				StkTypeMgr::instance().removeSuffix(iter->content);
			}

			shortlen = iter->content.size();
			rBuf.appendShort(shortlen);

			CUnit::getStockData(iter->content, rBuf);

			++rRetNum;
			++iter;
		}
	}
	else
	{
		short shortlen = 0;
		AtomRecordDB  atomData;
		atomData.uname = pDownload->name;
		atomData.sort = pDownload->rCH.sort;
		atomData.group = pDownload->rCH.group;
		if (1 == atomData.sort && 1 == atomData.group)
		{
			userDataExistInCloudDB(atomData);
			StkTypeMgr::instance().removeSuffix(atomData.content);
		}
		else
		{
			getCombinationData(atomData);
		}

		if (atomData.ver > pDownload->rCH.ver)
		{
			char st = (char)(atomData.sort);
			char gp = (char)(atomData.group);
			rBuf.appendChar(st);
			rBuf.appendChar(gp);
			rBuf.appendInt64(atomData.ver);
			rBuf.appendShort(shortlen);

			shortlen = atomData.content.size();
			rBuf.appendShort(shortlen);

			CUnit::getStockData(atomData.content, rBuf);

			++rRetNum;
		}
		else
		{
			rRet = 6;
		}
	}

	if (rRet)
	{
		sprintf(errorStr, "%s: %d handle CMD_DOWNLOAD %d failed",  __FUNCTION__, __LINE__, pCmd->tCmd);
		return errorResponse(&(pCmd->header), &(pCmd->subHeader), pBind, rRet, errorStr);
	}

	//返回数据
	sub_head rSub_head;
	memset(&rSub_head,0,sizeof(sub_head));
	rSub_head.sub_type = pDownload->subHeader.sub_type;
	rSub_head.sub_length = sizeof(char) + sizeof(char) + rBuf.Size();

	ACC_CMDHEAD rHead;
	memset(&rHead,0,sizeof(ACC_CMDHEAD));
	rHead.m_wCmdType = pDownload->header.m_wCmdType;
	rHead.m_nExpandInfo = pDownload->header.m_nExpandInfo;
	rHead.m_nLen = rSub_head.sub_length + sizeof(sub_head);

	pBind->SBuf.append(&rHead,sizeof(ACC_CMDHEAD));
	pBind->SBuf.append(&rSub_head,sizeof(sub_head));
	pBind->SBuf.appendChar(rRet);
	pBind->SBuf.appendChar(rRetNum);
	pBind->SBuf.append(rBuf.Data(),rBuf.Size());

	struct epoll_event ev;
	ev.data.ptr = pBind;
	ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
	epoll_ctl(m_epfd, EPOLL_CTL_MOD, pBind->fd, &ev);

	g_log.Wlog(3, "%s: download stock success!!!\n", __FUNCTION__);
	return true;
}

bool  CWorkThread::queryStockData4Upload(_Atom4Upload& atomData, std::set<_SymbolStock>& sSet)
{
	if (1 == atomData.updata.sort && 1 == atomData.updata.group)
	{
		std::string db("group0");
		return  userDataExistInCloudDB(atomData.updata, db, sSet);
	}
	else
	{
		return getCombinationData(atomData.updata, sSet);
	}

	return false;
}

bool  CWorkThread::getCombinationData(AtomRecordDB& cond)
{
	bson query;
	bson_init(&query);
	{
		bson_append_string(&query, "UName", cond.uname.c_str());
		bson_append_int(&query, "sort", cond.sort);
		bson_append_int(&query, "group", cond.group);
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_int(&field, "time", 1);
		bson_append_int(&field, "content", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	m_mongoSelf.getCursor(Config::instance().getStockDataDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	time_t   tm = 0;;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			cond.ver = bson_iterator_time_t(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson_iterator subIter[1];
			bson_iterator_subiterator(it, subIter);

			// scanner array object
			while (BSON_EOO != bson_iterator_next(subIter))
			{
				bson obj[1];
				bson_iterator_subobject(subIter, obj);

				bson_iterator  objIte[1];
				if (BSON_EOO != bson_find(objIte, obj, "data"))
				{
					stk = bson_iterator_string(objIte);

					tm = 0;
					if (BSON_EOO != bson_find(objIte, obj, "time"))
					{
						tm = bson_iterator_time_t(objIte);
					}

					cond.content.push_back(_SymbolStock(stk, tm));
				}
			}
		}

		flag = true;
	}

	if (!flag)
	{
		//TODO
		//check error flag for mongodb
	}

	mongo_cursor_destroy(&cursor);
	bson_destroy(&field);
	bson_destroy(&query);

	return flag;
}

bool  CWorkThread::getCombinationData(AtomRecordDB& cond, std::set<_SymbolStock>& sSet)
{
	bson query;
	bson_init(&query);
	{
		bson_append_string(&query, "UName", cond.uname.c_str());
		bson_append_int(&query, "sort", cond.sort);
		bson_append_int(&query, "group", cond.group);
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_int(&field, "content", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	m_mongoSelf.getCursor(Config::instance().getStockDataDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	time_t   tm = 0;;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];
		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson_iterator subIter[1];
			bson_iterator_subiterator(it, subIter);

			// scanner array object
			while (BSON_EOO != bson_iterator_next(subIter))
			{
				bson obj[1];
				bson_iterator_subobject(subIter, obj);

				bson_iterator  objIte[1];
				if (BSON_EOO != bson_find(objIte, obj, "data"))
				{
					stk = bson_iterator_string(objIte);

					tm = 0;
					if (BSON_EOO != bson_find(objIte, obj, "time"))
					{
						tm = bson_iterator_time_t(objIte);
					}

					sSet.insert(_SymbolStock(stk, tm));
				}
			}
		}

		flag = true;
	}

	if (!flag)
	{
		//TODO
		//check error flag for mongodb
	}

	mongo_cursor_destroy(&cursor);
	bson_destroy(&field);
	bson_destroy(&query);

	return flag;
}

bool  CWorkThread::userDataExistInCloudDB(AtomRecordDB& cond)
{
	bson  query;
	bson_init(&query);
	{
		bson_append_string(&query, "userid", cond.uname.c_str());
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_int(&field, "time", 1);
		bson_append_int(&field, "content", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	m_mongoCloud.getCursor(Config::instance().getSyncronizeDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	time_t tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			cond.ver = bson_iterator_time_t(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson  subobj[1];
			bson_iterator_subobject(it, subobj);

			bson_iterator  subIter[1];
			if (BSON_EOO != bson_find(subIter, subobj, "group0"))
			{
				bson_iterator  subsubIter[1];
				bson_iterator_subiterator(subIter, subsubIter);
				while (MONGO_OK == bson_iterator_next(subsubIter))
				{
					bson  desObj[1];
					bson_iterator_subobject(subsubIter, desObj);

					bson_iterator  stkIte[1];
					if (BSON_EOO != bson_find(stkIte, desObj, "data"))
					{
						stk = bson_iterator_string(stkIte);
						tm = 0;
						if (BSON_EOO != bson_find(stkIte, desObj, "time"))
						{
							tm = bson_iterator_time_t(stkIte);
						}
						cond.content.push_back(_SymbolStock(stk, tm));
					}
				}
			}
		}

		flag = true;
	}

	if (!flag)
	{
		//TODO
	}

	mongo_cursor_destroy(&cursor);
	bson_destroy(&field);
	bson_destroy(&query);

	return flag;
}

bool  CWorkThread::userDataExistInCloudDB(AtomRecordDB& cond, std::string& grp, std::set<_SymbolStock>& sSet)
{
	bson  query;
	bson_init(&query);
	{
		bson_append_string(&query, "userid", cond.uname.c_str());
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_int(&field, "content", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	m_mongoCloud.getCursor(Config::instance().getSyncronizeDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	time_t tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];
		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson  subobj[1];
			bson_iterator_subobject(it, subobj);

			bson_iterator  subIter[1];
			if (BSON_EOO != bson_find(subIter, subobj, grp.c_str()))
			{
				bson_iterator  subsubIter[1];
				bson_iterator_subiterator(subIter, subsubIter);
				while (MONGO_OK == bson_iterator_next(subsubIter))
				{
					bson  desObj[1];
					bson_iterator_subobject(subsubIter, desObj);

					bson_iterator  stkIte[1];
					if (BSON_EOO != bson_find(stkIte, desObj, "data"))
					{
						stk = bson_iterator_string(stkIte);
						tm = 0;
						if (BSON_EOO != bson_find(stkIte, desObj, "time"))
						{
							tm = bson_iterator_time_t(stkIte);
						}
						sSet.insert(_SymbolStock(stk, tm));
					}
				}
			}
		}

		flag = true;
	}

	if (!flag)
	{
		//TODO
	}

	mongo_cursor_destroy(&cursor);
	bson_destroy(&field);
	bson_destroy(&query);

	return flag;
}

bool  CWorkThread::addStk2DB(const AtomRecordDB& atomData)
{
	bson query;
	bson_init(&query);
	{
		if (1 == atomData.sort && 1 == atomData.group)
		{
			bson_append_string(&query, "userid", atomData.uname.c_str());
		}
		else
		{
			bson_append_string(&query, "UName", atomData.uname.c_str());
			bson_append_int(&query, "sort", atomData.sort);
			bson_append_int(&query, "group", atomData.group);
		}
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_start_object(&field, "$pushAll");
		{
			std::string  des;
			if (1 == atomData.sort && 1 == atomData.group)
			{
				des.assign("content.group0");
			}
			else
			{
				des.assign("content");
			}

			bson_append_start_array(&field, des.c_str());
			{
				int i = 0;
				char  buf[64];
				std::vector<_SymbolStock>::const_iterator iter = atomData.content.begin();
				while (iter != atomData.content.end())
				{
					bzero(buf, 64);
					sprintf(buf, "%d", i);

					bson_append_start_object(&field, buf);
					{
						bson_append_string(&field, "data", iter->stk.c_str());
						bson_append_time_t(&field, "time", iter->tm);
					}
					bson_append_finish_object(&field);

					++i;
					++iter;
				}
			}
			bson_append_finish_array(&field);
		}
		bson_append_finish_object(&field);

		bson_append_start_object(&field, "$set");
		{
			bson_append_time_t(&field, "time", atomData.ver);
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	bool flag = false;
	if (1 == atomData.sort && 1 == atomData.group)
	{
		flag = m_mongoCloud.mongoUpdate(Config::instance().getSyncronizeDB(), &query, &field);
	}
	else
	{
		flag = m_mongoSelf.mongoUpdate(Config::instance().getStockDataDB(), &query, &field);
	}

	bson_destroy(&query);
	bson_destroy(&field);

	return flag;
}

bool  CWorkThread::delStkInDB(const AtomRecordDB& atomData)
{
	bson query;
	bson_init(&query);
	{
		if (1 == atomData.sort && 1 == atomData.group)
		{
			bson_append_string(&query, "userid", atomData.uname.c_str());
		}
		else
		{
			bson_append_string(&query, "UName", atomData.uname.c_str());
			bson_append_int(&query, "sort", atomData.sort);
			bson_append_int(&query, "group", atomData.group);
		}
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_start_object(&field, "$pullAll");
		{
			std::string  des;
			if (1 == atomData.sort && 1 == atomData.group)
			{
				des.assign("content.group0");
			}
			else
			{
				des.assign("content");
			}

			bson_append_start_array(&field, des.c_str());
			{
				int i = 0;
				char  buf[64];
				std::vector<_SymbolStock>::const_iterator iter = atomData.content.begin();
				while (iter != atomData.content.end())
				{
					bzero(buf, 64);
					sprintf(buf, "%d", i);

					bson_append_start_object(&field, buf);
					{
						bson_append_string(&field, "data", iter->stk.c_str());
						bson_append_time_t(&field, "time", iter->tm);
					}
					bson_append_finish_object(&field);

					++i;
					++iter;
				}
			}
			bson_append_finish_array(&field);
		}
		bson_append_finish_object(&field);

		bson_append_start_object(&field, "$set");
		{
			bson_append_time_t(&field, "time", atomData.ver);
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	bool flag = false;
	if (1 == atomData.sort && 1 == atomData.group)
	{
		flag = m_mongoCloud.mongoUpdate(Config::instance().getSyncronizeDB(), &query, &field);
	}
	else
	{
		flag = m_mongoSelf.mongoUpdate(Config::instance().getStockDataDB(), &query, &field);
	}

	bson_destroy(&query);
	bson_destroy(&field);

	return flag;
}

bool  CWorkThread::setStkOfRecInDB(const AtomRecordDB& atomData)
{
	bson query;
	bson_init(&query);
	{
		if (1 == atomData.sort && 1 == atomData.group)
		{
			bson_append_string(&query, "userid", atomData.uname.c_str());
		}
		else
		{
			bson_append_string(&query, "UName", atomData.uname.c_str());
			bson_append_int(&query, "sort", atomData.sort);
			bson_append_int(&query, "group", atomData.group);
		}
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_start_object(&field, "$set");
		{
			std::string  des;
			if (1 == atomData.sort && 1 == atomData.group)
			{
				des.assign("content.group0");
			}
			else
			{
				des.assign("content");
			}

			bson_append_start_array(&field, des.c_str());
			{
				int i = 0;
				char  buf[64];
				std::vector<_SymbolStock>::const_iterator iter = atomData.content.begin();
				while (iter != atomData.content.end())
				{
					bzero(buf, 64);
					sprintf(buf, "%d", i);

					bson_append_start_object(&field, buf);
					{
						bson_append_string(&field, "data", iter->stk.c_str());
						bson_append_time_t(&field, "time", iter->tm);
					}
					bson_append_finish_object(&field);

					++i;
					++iter;
				}
			}
			bson_append_finish_array(&field);

			bson_append_time_t(&field, "time", atomData.ver);
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	bool flag = false;
	if (1 == atomData.sort && 1 == atomData.group)
	{
		flag = m_mongoCloud.mongoUpdate(Config::instance().getSyncronizeDB(), &query, &field);
	}
	else
	{
		flag = m_mongoSelf.mongoUpdate(Config::instance().getStockDataDB(), &query, &field);
	}

	bson_destroy(&query);
	bson_destroy(&field);

	return flag;
}

bool CWorkThread::updateStockData(std::vector<_Atom4Upload>& atomData)
{
	bool flag = false;
	std::vector<_Atom4Upload>::iterator iter = atomData.begin();
	while (iter != atomData.end())
	{
		if (0 == iter->act)
		{
			flag = addStk2DB(iter->updata);
		}
		else if (1 == iter->act)
		{
			flag = addStk2DB(iter->updata);
		}
		else if (2 == iter->act)
		{
			flag = addStk2DB(iter->updata);
		}
		else
		{
			flag = false;
		}

		if (!flag)
		{
			return flag;
		}
		++iter;
	}

	return true;
}

// for action==>0
bool  CWorkThread::addChangeData2Set(_Atom4Upload& atomData, std::vector<std::string>& stkSet,
									const std::set<_SymbolStock>& sSet, int&  isChange)
{
	std::vector<std::string>::iterator iter = stkSet.begin();
	while (iter != stkSet.end())
	{
		_SymbolStock  tmp(*iter);
		if (sSet.find(tmp) == sSet.end())
		{
			atomData.updata.content.push_back(tmp);
			isChange = 1;
		}
		++iter;
	}
	return true;
}

// for action==>1
bool  CWorkThread::addDelData2Set(_Atom4Upload& atomData, std::vector<std::string>& stkSet,
										const std::set<_SymbolStock>& sSet, int&  isChange)
{
	std::set<_SymbolStock>::iterator it;
	std::vector<std::string>::iterator iter = stkSet.begin();
	while (iter != stkSet.end())
	{
		_SymbolStock  tmp(*iter);
		it = sSet.find(tmp);
		if (it != sSet.end())
		{
			atomData.updata.content.push_back(*it);
			isChange = 1;
		}
		++iter;
	}
	return true;
}

// for action==>2
bool  CWorkThread::appendChangeData2Set(_Atom4Upload& atomData, const std::vector<std::string>& stkSet)
{
	atomData.updata.content.clear();

	std::vector<std::string>::const_iterator iter = stkSet.begin();
	while (iter != stkSet.end())
	{
		atomData.updata.content.push_back(_SymbolStock(*iter));
		++iter;
	}
	return true;
}

bool CWorkThread::Cmd_Upload(FoundBase*  pCmd, BindPtr* pBind)
{
	g_log.Wlog(3, "%s: %d starting!!!\n", __FUNCTION__, __LINE__);
	if (!pCmd || !pBind)
	{
		return false;
	}

	FoundUpload*  pUpload = dynamic_cast<FoundUpload*>(pCmd);
	if (!pUpload)
	{
		return false;
	}

	char errorStr[2048] = { 0 };
	std::vector<_Atom4Upload>  vRes;

	// for response flag
	char ret = 0;
	char rRetNum = 0;
	CDBuffer rBuf;

	// flag for update mongodb or not
	bool update_or_not = false;

	for (int i = 0; i < pUpload->row; ++i)
	{
		RowCacheHead RCH;

		_Atom4Upload  atomData;
		atomData.updata.uname = pUpload->name;
		atomData.updata.sort = pUpload->dt[i].sort;
		atomData.updata.group = pUpload->dt[i].group;
		atomData.act = pUpload->dt[i].action;

		int isChange = 0;
		if (0 == pUpload->dt[i].action)
		{
			std::set<_SymbolStock>  sSet;
			queryStockData4Upload(atomData, sSet);
			addChangeData2Set(atomData, pUpload->dt[i].stock, sSet, isChange);
		}
		else if (1 == pUpload->dt[i].action)
		{
			std::set<_SymbolStock>  sSet;
			queryStockData4Upload(atomData, sSet);
			addDelData2Set(atomData, pUpload->dt[i].stock, sSet, isChange);
		}
		else if ( 2 == pUpload->dt[i].action)
		{
			appendChangeData2Set(atomData, pUpload->dt[i].stock);
			isChange = 1;
		}
		else
		{
			ret = 3;
			g_log.Wlog(1, "invalid action: %d!!!\n", pUpload->dt[i].action);
			break;
		}

		if (isChange)
		{
			update_or_not = true;
			atomData.updata.ver = CUnit::US();
			vRes.push_back(atomData);
		}

		RCH.sort = atomData.updata.sort;
		RCH.group = atomData.updata.group;
		RCH.ver = atomData.updata.ver;
		rBuf.append(&RCH, sizeof(RCH));

		++rRetNum;
	}

	if ( ret )
	{
		sprintf(errorStr, "error happened in cmd_upload, ret = %d, vRes's size = %d", ret, vRes.size());
		return errorResponse(&(pUpload->header), &(pUpload->subHeader), pBind, ret, errorStr);
	}

	if (update_or_not && !updateStockData(vRes))
	{
		sprintf(errorStr, "update mongodb failed in cmd_upload, ret = %d, vRes's size = %d", 3, vRes.size());
		return errorResponse(&(pUpload->header), &(pUpload->subHeader), pBind, 3, errorStr);
	}

	//返回数据
	sub_head rSub_head;
	memset(&rSub_head, 0, sizeof(sub_head));
	rSub_head.sub_type = pUpload->subHeader.sub_type;
	rSub_head.sub_length = sizeof(char) +sizeof(char) +rBuf.Size();

	ACC_CMDHEAD rHead;
	memset(&rHead, 0, sizeof(ACC_CMDHEAD));
	rHead.m_wCmdType = pUpload->header.m_wCmdType;
	rHead.m_nExpandInfo = pUpload->header.m_nExpandInfo;
	rHead.m_nLen = rSub_head.sub_length + sizeof(sub_head);

	pBind->SBuf.append(&rHead, sizeof(ACC_CMDHEAD));
	pBind->SBuf.append(&rSub_head, sizeof(sub_head));
	pBind->SBuf.appendChar(0);
	pBind->SBuf.appendChar(rRetNum);
	pBind->SBuf.append(rBuf.Data(),rBuf.Size());

	struct epoll_event ev;
	ev.data.ptr = pBind;
	ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
	epoll_ctl(m_epfd, EPOLL_CTL_MOD, pBind->fd, &ev);

	g_log.Wlog(3, "%s: upload stock success!!!\n", __FUNCTION__);

	return true;
}

bool CWorkThread::switch2Cmd(Actor& pCmd)
{
	VStruct* pValue = dynamic_cast<VStruct*>(pCmd.sBase.get());
	char errorStr[1024] = { 0 };

	if (pValue)
	{
		if (CMD_UPLOAD == pCmd.fBase->tCmd)
		{
			return Cmd_Upload(pCmd.fBase.get(), (BindPtr*)(pValue->pCurr));
		}
		else if (CMD_DOWNLOAD == pCmd.fBase->tCmd)
		{
			return Cmd_Download(pCmd.fBase.get(), (BindPtr*)(pValue->pCurr));
		}
		else
		{
			sprintf(errorStr, "%s: %d handle %d error cmd type", __FUNCTION__, __LINE__, pCmd.fBase->tCmd);
			return errorResponse(&(pCmd.fBase->header), &(pCmd.fBase->subHeader), (BindPtr*)pValue->pCurr, 7, errorStr);
		}
	}

	return true;
}

bool  CWorkThread::updateUserInfoInDB(const char* name, const char* passwd)
{
	time_t  tm = time(0);

	bson query;
	bson_init(&query);
	{
		bson_append_string(&query, "UName", name);
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_start_object(&field, "$set");
		{
			bson_append_string(&field, "PWord", passwd);
			bson_append_time_t(&field, "SyncTime", tm);
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	bool flag = m_mongoSelf.mongoUpdate(Config::instance().getUserCacheDB(), &query, &field);

	bson_destroy(&field);
	bson_destroy(&query);

	return flag;
}

void CWorkThread::processAccCenterReturn()
{
	g_log.Wlog(5, "%s\n", __FUNCTION__);
	std::list<Actor>* pList = NULL;
	getAndResetListPtr(pList);

	if (!pList)
	{
		return;
	}

	int uIndex = 0;
	VStruct *pVS = NULL;
	char errorStr[256] = { 0 };

	AccUserCache User;
	std::list<Actor>::iterator iter = pList->begin();
	while (iter != pList->end())
	{
		pVS = dynamic_cast<VStruct*>(iter->sBase.get());
		if (pVS)
		{
			g_log.Wlog(3, "AccCenter return %s to CWorkThread success!!!\n", pVS->uname);
			//写缓存int UserMgr::AddUser( AccUserCache* pUser )
			strncpy(User.uname, pVS->uname, sizeof(User.uname) - 1);
			strncpy(User.passwd, pVS->passwd_md5, sizeof(User.passwd) - 1);
			User.synctime = m_CurrTime;

			if (pVS->bSend == 2 && strlen(User.passwd) > 0 )
			{
				uIndex = UserMgr::instance().FindUser(User.uname);
				if (uIndex >= 0)
				{
					UserMgr::instance().UpdateUser(uIndex, &User);
				}
				else
				{
					UserMgr::instance().AddUser(&User);
				}

				// update db
				updateUserInfoInDB(User.uname, User.passwd);

				if (!UserMgr::instance().CheckUser(pVS->uname, pVS->passwd))
				{
					switch2Cmd(*iter);
				}
				else
				{
					sprintf(errorStr, "%s(%d) user's password isn't correct!!!", __FUNCTION__, __LINE__);
					errorResponse(&(iter->fBase->header), &(iter->fBase->subHeader), (BindPtr*)pVS->pCurr, 1, errorStr);
				}
			}
			else
			{
				sprintf(errorStr, "%s: %d handle %d failed", __FUNCTION__, __LINE__, iter->fBase->tCmd);
				errorResponse(&(iter->fBase->header), &(iter->fBase->subHeader), (BindPtr*)pVS->pCurr, 1, errorStr);
			}
		}
		else
		{
			g_log.Wlog(1, "%s: %d handle %d failed", __FUNCTION__, __LINE__, iter->fBase->tCmd);
		}

		//处理返回
		pList->erase(iter++);
	}
	delete pList;
}

bool  CWorkThread::errorResponse(const ACC_CMDHEAD*  pHead, const sub_head* pSubHead, BindPtr*  pBind,
		const char res, const char* descript)
{
	if (!pHead || !pSubHead || !pBind || !descript)
	{
		return false;
	}

	//返回数据
	sub_head rSub_head;
	memset(&rSub_head, 0, sizeof(sub_head));
	rSub_head.sub_type = pSubHead->sub_type;
	rSub_head.sub_length = 1 ;

	ACC_CMDHEAD rHead;
	memset(&rHead, 0, sizeof(ACC_CMDHEAD));
	rHead.m_wCmdType = pHead->m_wCmdType;
	rHead.m_nExpandInfo = pHead->m_nExpandInfo;
	rHead.m_nLen = sizeof(sub_head) + rSub_head.sub_length;

	pBind->SBuf.append(&rHead,sizeof(ACC_CMDHEAD));
	pBind->SBuf.append(&rSub_head,sizeof(sub_head));
	pBind->SBuf.appendChar(res);

	struct epoll_event ev;
	ev.data.ptr = pBind;
	ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
	epoll_ctl(m_epfd, EPOLL_CTL_MOD, pBind->fd, &ev);

	g_log.Wlog( 3, "CWorkThread::%s failed. rRet=%d\n", descript, res);
	return true;
}

bool  CWorkThread::parseUpload()
{
	g_log.Wlog(5, "%s", __FUNCTION__);
	char* pData = (char*)(m_pCurr->RBuf.Data());

	FoundUpload*   ptrUpload = new FoundUpload();
	ptrUpload->tCmd = CMD_UPLOAD;

	ACC_CMDHEAD* pHead = (ACC_CMDHEAD*)(pData);
	sub_head* pSub_head = (sub_head*)( pHead + 1 );
	size_t PackLen = sizeof(ACC_CMDHEAD) + pHead->m_nLen;

	// ----------------------ACC_CMDHEAD--------------------
	memcpy(&(ptrUpload->header), pData, sizeof(ACC_CMDHEAD));
	size_t nPos = sizeof(ACC_CMDHEAD);

	//----------------------sub_head----------------------
	memcpy(&(ptrUpload->subHeader), pData + nPos, sizeof(sub_head));
	nPos += sizeof(sub_head);

	// ----------------------uname----------------------
	if (!getNstring(pData, PackLen, ACC_MAXUNAME, &nPos, ptrUpload->name) || strlen(ptrUpload->name) < 3)
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
	}

	//----------------------passwd------------------------
	if (!getNstring(pData, PackLen, ACC_MAXUPASSWD, &nPos, ptrUpload->passwd))
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
	}

	if (!Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr)) && strlen(ptrUpload->passwd) < 3)
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
	}

	// ---------------------row num------------------------
	if (!getChar(pData, PackLen, &nPos, &(ptrUpload->row)) || ptrUpload->row <= 0 || ptrUpload->row > Config::instance().getMaxRow())
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
	}

	int SizeOfNstring = sizeof(nString);
	char tmpstock[1024] = {0};
	ptrUpload->dt.resize(ptrUpload->row);
	bool needExchange = false;
	for (int i = 0; i < ptrUpload->row; ++i)
	{
		//--------------sort--------------
		if (!getChar(pData, PackLen, &nPos, &(ptrUpload->dt[i].sort)) || !ptrUpload->dt[i].sort)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
		}
		//---------------group-------------
		if (!getChar(pData, PackLen, &nPos, &(ptrUpload->dt[i].group)) || !ptrUpload->dt[i].group)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
		}
		//---------------action-------------
		if (!getChar(pData, PackLen, &nPos, &(ptrUpload->dt[i].action))
			|| ptrUpload->dt[i].action < 0 || ptrUpload->dt[i].action > 2)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
		}
		//---------------stock num-------------
		short arrNum = 0;
		if (!getShort(pData, PackLen, &nPos, &arrNum) || arrNum < 0 || arrNum > 100)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
		}

		//----------------stock---------------
		needExchange = ((1 == ptrUpload->dt[i].sort) && (1 == ptrUpload->dt[i].group));
		for (int j = 0; j < arrNum; ++j)
		{
			int failPos = nPos;
			memset(tmpstock, 0, sizeof(tmpstock));
			if (!getNstring(pData, PackLen, sizeof(tmpstock), &nPos, tmpstock))
			{
				nPos = failPos;
				nString* pNStr = (nString*)(pData + nPos);
				nPos +=  SizeOfNstring + pNStr->len;
				continue;
			}

			std::string stk(tmpstock);
			if (needExchange)
			{
				StkTypeMgr::instance().addSuffix(stk);
			}

			if (ptrUpload->dt[i].filter.find(stk) == ptrUpload->dt[i].filter.end())
			{
				++ptrUpload->dt[i].stockNum;
				ptrUpload->dt[i].stock.push_back(stk);
				ptrUpload->dt[i].filter.insert(stk);
			}
		}
	}

	if (!Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr))
			&& UserMgr::instance().CheckUser(ptrUpload->name, ptrUpload->passwd))
	{
		// async send to AccCenter
		if (!m_ChkIsFirst.IsFirstOrTimeOuted(ptrUpload->name))
		{
			g_log.Wlog(1, "CWorkThread::parseUpload m_ChkIsFirst.IsFirstOrTimeOuted(%s) failed.....\n", ptrUpload->name);
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 1, "CMD_UPLOAD");
		}

		VStruct* pVerify = new VStruct();
		pVerify->tAsyn = ASYNC_GET_USER_INFO;
		strcpy(pVerify->uname, ptrUpload->name);
		strcpy(pVerify->passwd, ptrUpload->passwd);
		pVerify->extra = pHead->m_nExpandInfo;
		pVerify->LoginTime = time(0);
		pVerify->bSend = 0;
		pVerify->pWorkThread = this;
		pVerify->pCurr = m_pCurr;

		Actor  pMsg;
		pMsg.fBase.reset(ptrUpload);
		pMsg.sBase.reset(pVerify);

		ThManage::instance().Post2AccCenter(pMsg);
		g_log.Wlog( 3, "CWorkThread::parseUpload into AccCenter.uname = %s\n",pVerify->uname);
	}
	else
	{
		Cmd_Upload(ptrUpload, m_pCurr);
		g_log.Wlog( 3, "CWorkThread::parseUpload begin process CMD_UPLOAD uname = %s\n", ptrUpload->name);
	}

	return true;
}

bool  CWorkThread::parseDownload()
{
	g_log.Wlog(5, "%s\n", __FUNCTION__);
	char* pData = (char*)(m_pCurr->RBuf.Data());

	FoundDownload*   ptrDownload = new FoundDownload();
	ptrDownload->tCmd = CMD_DOWNLOAD;

	ACC_CMDHEAD* pHead = (ACC_CMDHEAD*)(pData);
	sub_head* pSub_head = (sub_head*)( pHead + 1 );
	size_t PackLen = sizeof(ACC_CMDHEAD) + pHead->m_nLen;

	// ----------------------ACC_CMDHEAD--------------------
	memcpy(&(ptrDownload->header), pData, sizeof(ACC_CMDHEAD));
	size_t nPos = sizeof(ACC_CMDHEAD);

	//----------------------sub_head----------------------
	memcpy(&(ptrDownload->subHeader), pData + nPos, sizeof(sub_head));
	nPos += sizeof(sub_head);

	// ----------------------uname----------------------
	if (!getNstring(pData, PackLen, ACC_MAXUNAME, &nPos, ptrDownload->name) || strlen(ptrDownload->name) < 3)
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD");
	}

	//----------------------passwd------------------------
	if (!getNstring(pData, PackLen, ACC_MAXUPASSWD, &nPos, ptrDownload->passwd))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD");
	}

	if (!Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr)) && strlen(ptrDownload->passwd) < 3)
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD");
	}

	if (!getRowCacheHead(pData, PackLen, &nPos, &(ptrDownload->rCH)))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD");
	}


	if (!Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr))
			&& UserMgr::instance().CheckUser(ptrDownload->name, ptrDownload->passwd))
	{
		// async send to AccCenter
		if (!m_ChkIsFirst.IsFirstOrTimeOuted(ptrDownload->name))
		{
			g_log.Wlog(1, "CWorkThread::parseDownload m_ChkIsFirst.IsFirstOrTimeOuted(%s) failed.....\n", ptrDownload->name);
			delete ptrDownload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_DOWNLOAD");
		}

		VStruct* pVerify = new VStruct();
		pVerify->tAsyn = ASYNC_GET_USER_INFO;
		strcpy(pVerify->uname, ptrDownload->name);
		strcpy(pVerify->passwd, ptrDownload->passwd);
		pVerify->extra = pHead->m_nExpandInfo;
		pVerify->LoginTime = time(0);
		pVerify->bSend = 0;
		pVerify->pWorkThread = this;
		pVerify->pCurr = m_pCurr;

		Actor pMsg;
		pMsg.fBase.reset(ptrDownload);
		pMsg.sBase.reset(pVerify);

		ThManage::instance().Post2AccCenter(pMsg);
		g_log.Wlog( 3, "CWorkThread::parseDownload into AccCenter.uname = %s\n",pVerify->uname);
	}
	else
	{
		Cmd_Download(ptrDownload, m_pCurr);
		g_log.Wlog( 3, "CWorkThread::parseDownload begin process CMD_DOWNLOAD uname = %s\n",ptrDownload->name);
	}

	return true;
}

void CWorkThread::processPacket()
{
	ACC_CMDHEAD * pHead;
	sub_head * pSub_head;
	size_t PackLen;

	do
	{
		pHead = (ACC_CMDHEAD *)( m_pCurr->RBuf.Data() );
		if( m_pCurr->RBuf.Size() < sizeof(ACC_CMDHEAD) )
		{
			return;	//head没完整
		}

		PackLen = sizeof(ACC_CMDHEAD) + pHead->m_nLen ;
		if( m_pCurr->RBuf.Size() < PackLen )
		{
			return; //body没完整
		}

		if( pHead->m_wCmdType == ACCCMD_SERVERLOGIN )
		{
			ACC_SERVERLOGIN *_CurrServer = ( ACC_SERVERLOGIN * )( pHead + 1 );

			if( ( pHead->m_nLen == sizeof( struct ACC_SERVERLOGIN ) )
					&& ( !memcmp( _CurrServer->m_cValid, Config::instance().getValidString(), 8 ) ) )
			{
				g_log.Wlog( 3, "ServerLogin Success!_CurrServer->m_cValid=%s,ServerID=%d\n", _CurrServer->m_cValid, _CurrServer->m_nSerId );
				m_pCurr->bLogin = 1;
			}
		}

		if( pHead->m_wCmdType == ACCCMD_KEEPALIVE && m_pCurr->bLogin == 1 )
		{
			m_pCurr->SBuf.append( pHead, PackLen );
		}

		if ( pHead->m_wCmdType == Config::instance().getCmdType() && m_pCurr->bLogin == 1 )
		{
			pSub_head = ( sub_head *) ( pHead + 1 );
			switch (pSub_head->sub_type)
			{
				case SYNC_UPLOAD:
					parseUpload();
					break;
				case SYNC_DOWNLOAD:
					parseDownload();
					break;

				default:
					m_pCurr->SBuf.append( pHead, PackLen ); //Echo...
					break;
			}
		}

		m_pCurr->RBuf.CutHead( PackLen );
	}
	while( m_pCurr->RBuf.Size() > 0 );

	return;
}

bool CWorkThread::getNstring(const char* src, const size_t PackLen, const int bufLen, size_t* pos, void* des)
{
	if (!src || !pos || !des)
	{
		return false;
	}

	if (*pos + sizeof(nString) > PackLen)
	{
		return false;
	}
	nString* pNStr = (nString*)( src + *pos );
	*pos += sizeof(nString);

	if (*pos + pNStr->len > PackLen)
	{
		return false;
	}

	if( pNStr->len > 0 )
	{
		strncpy( (char*)des, pNStr->buf, pNStr->len > bufLen ? bufLen - 1 : pNStr->len );
	}

	*pos += pNStr->len;

	return true;
}

bool CWorkThread::getRowCacheHead(const char* src, const size_t PackLen, size_t* pos, RowCacheHead* des)
{
	if (!src || !pos || !des)
	{
		return false;
	}

	if (*pos + sizeof(RowCacheHead) > PackLen)
	{
		return false;
	}

	memcpy(des, src + *pos, sizeof(RowCacheHead));
	*pos += sizeof(RowCacheHead);

	return true;
}

bool CWorkThread::getChar(const char* src, const size_t PackLen, size_t* pos, char* des)
{
	if (!src || !pos || !des)
	{
		return false;
	}

	if (*pos + sizeof(char) > PackLen)
	{
		return false;
	}

	*des = *((char*)(src + *pos));
	*pos += sizeof(char);

	return true;
}

bool CWorkThread::getShort(const char* src, const size_t PackLen, size_t* pos, short* des)
{
	if (!src || !pos || !des)
	{
		return false;
	}

	if(*pos + sizeof(short) > PackLen )
	{
		return false;
	}
	*des = *((short*)(src + *pos));
	*pos += sizeof(short);

	return true;
}

