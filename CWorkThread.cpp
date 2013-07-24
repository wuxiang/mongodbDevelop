#include "CWorkThread.h"
#include "g_function.h"
#include "stopAllThread.h"

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
	m_mongoSelf.setMongoInfo(Config::instance().getMongosInfo(), NULL, NULL);
	if (!m_mongoSelf.mongoConnect())
	{
		g_log.Wlog(0, "connect to mongodb failed when start CWorkThread!!!");
		exit(0);
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
			m_PreCheckTime = m_CurrTime;
		}

		if (!isRuning())
		{
			StopAllThread::instance().incIdx();
			return;
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
	uint64_t   tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		AtomRecordDB  tmpRec;
		tmpRec.uname = name;

		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "sort"))
		{
			tmpRec.sort = (uint8_t)bson_iterator_int(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "group"))
		{
			tmpRec.group = (uint8_t)bson_iterator_int(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			tmpRec.ver = bson_iterator_long(it);
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
						tm = bson_iterator_long(objIte);
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
		bson_append_int(&field, "content", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	m_mongoSelf.getCursor(Config::instance().getSyncronizeDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	uint64_t tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		AtomRecordDB  tmpRec;
		tmpRec.uname = name;
		tmpRec.sort = 1;
		tmpRec.group = 1;

		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			tmpRec.ver = bson_iterator_long(it);
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
				while (BSON_EOO != bson_iterator_next(subsubIter))
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
							tm = bson_iterator_long(stkIte);
						}

						tmpRec.content.push_back(_SymbolStock(stk, tm));
					}
				}
			}
		}

		vRes.push_back(tmpRec);
		flag = true;
		break;
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
	flag = queryAllStockFromDB(name, vRes) || flag;
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
		std::vector<AtomRecordDB>  vRes;
		m_mongoSelf.queryAllStockData(pDownload->name, std::string(), Config::instance().getStockDataDB(), vRes);
		m_mongoSelf.queryAllStockData(pDownload->name, std::string(), Config::instance().getSyncronizeDB(), vRes);
		std::vector<AtomRecordDB>::iterator iter = vRes.begin();
		while (iter != vRes.end())
		{
			short shortlen = 0;
			uint8_t st = (uint8_t)(iter->sort);
			uint8_t gp = (uint8_t)(iter->group);
			rBuf.append(&st, sizeof(uint8_t));
			rBuf.append(&gp, sizeof(uint8_t));
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

		if (vRes.size() <= 0)
		{
			rRet = 2;
		}
	}
	else
	{
		bool executeflag = false;
		short shortlen = 0;
		AtomRecordDB  atomData;
		atomData.uname = pDownload->name;
		atomData.sort = pDownload->rCH.sort;
		atomData.group = pDownload->rCH.group;
		if (1 == atomData.sort && 1 == atomData.group)
		{
			if (userDataExistInCloudDB(atomData))
			{
				executeflag = true;
				StkTypeMgr::instance().removeSuffix(atomData.content);
			}
		}
		else
		{
			executeflag = getCombinationData(atomData);
		}

		if (executeflag)
		{
			if (atomData.ver > pDownload->rCH.ver)
			{
				uint8_t st = (uint8_t)(atomData.sort);
				uint8_t gp = (uint8_t)(atomData.group);
				rBuf.append(&st, sizeof(uint8_t));
				rBuf.append(&gp, sizeof(uint8_t));
				rBuf.appendInt64(atomData.ver);
				rBuf.appendShort(shortlen);

				shortlen = atomData.content.size();
				rBuf.appendShort(shortlen);

				CUnit::getStockData(atomData.content, rBuf);

				++rRetNum;
			}
			else
			{
				rRet = 1;
			}
		}
		else
		{
			rRet = 2;
		}
	}

	if (rRet)
	{
		sprintf(errorStr, "%s: %d handle CMD_DOWNLOAD %d ",  __FUNCTION__, __LINE__, pCmd->tCmd);
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
	//fprintf(stderr, "%s end time %lld\n", __FUNCTION__, CUnit::US());
	return true;
}

bool  CWorkThread::queryStockData4Upload(AtomRecordDB& atomData)
{
	if (1 == atomData.sort && 1 == atomData.group)
	{
		return  userDataExistInCloudDB(atomData);
	}
	else
	{
		return getCombinationData(atomData);
	}

	return false;
}

bool  CWorkThread::getCombinationData(AtomRecordDB& cond)
{
	bson query;
	bson_init(&query);
	{
		bson_append_string(&query, "UName", cond.uname.c_str());
		bson_append_string(&query, "macIP", cond.macIP.c_str());
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
	uint64_t   tm = 0;;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			cond.ver = bson_iterator_long(it);
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
						tm = bson_iterator_long(objIte);
					}

					cond.content.push_back(_SymbolStock(stk, tm));
				}
			}
		}

		flag = true;
		break;
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
		bson_append_string(&query, "macIP", cond.macIP.c_str());
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
	uint64_t   tm = 0;;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			cond.ver = bson_iterator_long(it);
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
						tm = bson_iterator_long(objIte);
					}

					sSet.insert(_SymbolStock(stk, tm));
				}
			}
		}

		flag = true;
		break;
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
		if (cond.macIP.length())
		{
			bson_append_string(&query, "macIP", cond.macIP.c_str());
		}
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
	m_mongoSelf.getCursor(Config::instance().getSyncronizeDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	uint64_t tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			cond.ver = bson_iterator_long(it);
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
				while (BSON_EOO != bson_iterator_next(subsubIter))
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
							tm = bson_iterator_long(stkIte);
						}
						cond.content.push_back(_SymbolStock(stk, tm));
					}
				}
			}
		}

		flag = true;
		break;
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
		if (cond.macIP.length())
		{
			bson_append_string(&query, "macIP", cond.macIP.c_str());
		}
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
	m_mongoSelf.getCursor(Config::instance().getSyncronizeDB(), &query, &field, &cursor);

	bool flag = false;
	std::string stk;
	uint64_t tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			cond.ver = bson_iterator_long(it);
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson  subobj[1];
			bson_iterator_subobject(it, subobj);

			bson_iterator  subIter[1];
			if (BSON_EOO != bson_find(subIter, subobj, grp.c_str()))
			{
				bson_iterator  subsubIter[1];
				bson_iterator_subiterator(subIter, subsubIter);
				while (BSON_EOO != bson_iterator_next(subsubIter))
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
							tm = bson_iterator_long(stkIte);
						}
						sSet.insert(_SymbolStock(stk, tm));
					}
				}
			}
		}

		flag = true;
		break;
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
	if (!atomData.uname.length() && !atomData.macIP.length())
	{
		return false;
	}

	bson query;
	bson_init(&query);
	{
		if (1 == atomData.sort && 1 == atomData.group)
		{
			bson_append_string(&query, "userid", atomData.uname.c_str());
			if (atomData.macIP.length())
			{
				bson_append_string(&query, "macIP", atomData.macIP.c_str());
			}
		}
		else
		{
			bson_append_string(&query, "UName", atomData.uname.c_str());
			bson_append_int(&query, "sort", atomData.sort);
			bson_append_int(&query, "group", atomData.group);
			bson_append_string(&query, "macIP", atomData.macIP.c_str());
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
						bson_append_long(&field, "time", iter->tm);
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
			bson_append_long(&field, "time", atomData.ver);

			if (1 == atomData.sort && 1 == atomData.group)
			{
				bson_append_string(&field, "fileName", "mystock0.json");
			}
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	bool flag = false;
	if (1 == atomData.sort && 1 == atomData.group)
	{
		flag = m_mongoSelf.mongoUpdate(Config::instance().getSyncronizeDB(), &query, &field);
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
	if (!atomData.uname.length() && !atomData.macIP.length())
	{
		return false;
	}

	bson query;
	bson_init(&query);
	{
		if (1 == atomData.sort && 1 == atomData.group)
		{
			bson_append_string(&query, "userid", atomData.uname.c_str());
			if (atomData.macIP.length())
			{
				bson_append_string(&query, "macIP", atomData.macIP.c_str());
			}
		}
		else
		{
			bson_append_string(&query, "UName", atomData.uname.c_str());
			bson_append_int(&query, "sort", atomData.sort);
			bson_append_int(&query, "group", atomData.group);
			bson_append_string(&query, "macIP", atomData.macIP.c_str());
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
						bson_append_long(&field, "time", iter->tm);
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
			bson_append_long(&field, "time", atomData.ver);
			if (1 == atomData.sort && 1 == atomData.group)
			{
				bson_append_string(&field, "fileName", "mystock0.json");
			}
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	bool flag = false;
	if (1 == atomData.sort && 1 == atomData.group)
	{
		flag = m_mongoSelf.mongoUpdate(Config::instance().getSyncronizeDB(), &query, &field);
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
			if (atomData.macIP.length())
			{
				bson_append_string(&query, "macIP", atomData.macIP.c_str());
			}
		}
		else
		{
			bson_append_string(&query, "UName", atomData.uname.c_str());
			bson_append_int(&query, "sort", atomData.sort);
			bson_append_int(&query, "group", atomData.group);
			bson_append_string(&query, "macIP", atomData.macIP.c_str());
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
						bson_append_long(&field, "time", iter->tm);
					}
					bson_append_finish_object(&field);

					++i;
					++iter;
				}
			}
			bson_append_finish_array(&field);

			bson_append_long(&field, "time", atomData.ver);

			if (1 == atomData.sort && 1 == atomData.group)
			{
				bson_append_string(&field, "fileName", "mystock0.json");
			}
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	bool flag = false;
	if (1 == atomData.sort && 1 == atomData.group)
	{
		flag = m_mongoSelf.mongoUpdate(Config::instance().getSyncronizeDB(), &query, &field);
	}
	else
	{
		flag = m_mongoSelf.mongoUpdate(Config::instance().getStockDataDB(), &query, &field);
	}

	bson_destroy(&query);
	bson_destroy(&field);

	return flag;
}

bool CWorkThread::updateStockData(std::vector<_Atom4Upload>& atomData, std::set<std::string>& sDev)
{
	bool flag = false;
	std::set<std::string>::iterator itDev;
	std::vector<_Atom4Upload>::iterator iter = atomData.begin();
	while (iter != atomData.end())
	{
		itDev = sDev.begin();
		while ( itDev != sDev.end())
		{
			iter->updata.macIP = *itDev;

			if (0 == iter->act)
			{
				flag = addStk2DB(iter->updata);
			}
			else if (1 == iter->act)
			{
				flag = delStkInDB(iter->updata);
			}
			else if (2 == iter->act)
			{
				flag = setStkOfRecInDB(iter->updata);
			}
			else
			{
				flag = false;
			}

			if (!flag)
			{
				return flag;
			}

			++itDev;
		}
		++iter;
	}

	return true;
}

// for action==>0
bool  CWorkThread::addChangeData2Set(AtomRecordDB& atomData, const std::vector<std::string>& stkSet, int&  isChange)
{
	std::vector<std::string>::const_iterator iter = stkSet.begin();
	while (iter != stkSet.end())
	{
		_SymbolStock  tmp(*iter);
		if (std::find(atomData.content.begin(), atomData.content.end(), tmp) == atomData.content.end())
		{
			atomData.content.push_back(tmp);
			isChange = 1;
		}
		++iter;
	}
	return true;
}

// for action==>1
bool  CWorkThread::addDelData2Set(AtomRecordDB& atomData, const std::vector<std::string>& stkSet, int&  isChange)
{
	std::vector<_SymbolStock>::iterator it;
	std::vector<std::string>::const_iterator iter = stkSet.begin();
	while (iter != stkSet.end())
	{
		_SymbolStock  tmp(*iter);
		it = std::find(atomData.content.begin(), atomData.content.end(), tmp);
		if (it != atomData.content.end())
		{
			atomData.content.erase(it);
			isChange = 1;
		}
		++iter;
	}
	return true;
}

// for action==>2
bool  CWorkThread::appendChangeData2Set(AtomRecordDB& atomData, const std::vector<std::string>& stkSet)
{
	atomData.content.clear();

	std::vector<std::string>::const_iterator iter = stkSet.begin();
	while (iter != stkSet.end())
	{
		atomData.content.push_back(_SymbolStock(*iter));
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

	std::set<std::string>  sDev;
	m_mongoSelf.theNumberOfDevOnAccount(std::string(pUpload->name), sDev);
	sDev.insert(std::string(""));

	for (int i = 0; i < pUpload->row; ++i)
	{
		RowCacheHead RCH;

		AtomRecordDB  atomData;
		atomData.uname = pUpload->name;
		atomData.sort = pUpload->dt[i].sort;
		atomData.group = pUpload->dt[i].group;

		int isChange = 0;
		if (0 == pUpload->dt[i].action)
		{
			queryStockData4Upload(atomData);
			addChangeData2Set(atomData, pUpload->dt[i].stock, isChange);
		}
		else if (1 == pUpload->dt[i].action)
		{
			queryStockData4Upload(atomData);
			addDelData2Set(atomData, pUpload->dt[i].stock, isChange);
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
			bool succFlag = true;
			atomData.ver = CUnit::US();
			std::set<std::string>::const_iterator itDev =  sDev.begin();
			while (itDev != sDev.end())
			{
				atomData.macIP = *itDev;
				succFlag = succFlag && setStkOfRecInDB(atomData);

				if (!succFlag) break;
				++itDev;
			}

			if (!succFlag)
			{
				ret = 3;
				g_log.Wlog(1, "add update Stock to mongodb failed!!!\n");
				break;
			}
		}

		RCH.sort = atomData.sort;
		RCH.group = atomData.group;
		RCH.ver = atomData.ver;
		rBuf.append(&RCH, sizeof(RCH));

		++rRetNum;
	}

	if ( ret )
	{
		sprintf(errorStr, "error happened in cmd_upload, ret = %d, vRes's size = %ld", ret, vRes.size());
		return errorResponse(&(pUpload->header), &(pUpload->subHeader), pBind, ret, errorStr);
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

	//fprintf(stderr, "%s end time %lld\n", __FUNCTION__, CUnit::US());
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
		else if (CMD_UPLOAD4ALLINFO == pCmd.fBase->tCmd)
		{
			return Cmd_Upload4AllInfo(pCmd.fBase.get(), (BindPtr*)(pValue->pCurr));
		}
		else if (CMD_DOWNLOAD4ALLINFO == pCmd.fBase->tCmd)
		{
			return Cmd_Download4AllInfo(pCmd.fBase.get(), (BindPtr*)(pValue->pCurr));
		}
		else
		{
			sprintf(errorStr, "%s: %d handle %d error cmd type", __FUNCTION__, __LINE__, pCmd.fBase->tCmd);
			return errorResponse(&(pCmd.fBase->header), &(pCmd.fBase->subHeader), (BindPtr*)pValue->pCurr, 3, errorStr);
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
			bson_append_long(&field, "SyncTime", tm);
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

				//if (!UserMgr::instance().CheckUser(pVS->uname, pVS->passwd))
				if(!PassCMP(User.passwd, pVS->passwd))
				{
					switch2Cmd(*iter);
				}
				else
				{
					int errFlag =  (iter->fBase->tCmd == CMD_UPLOAD || iter->fBase->tCmd == CMD_UPLOAD4ALLINFO ) ? 1 : 3;
					sprintf(errorStr, "%s(%d) user's password isn't correct!!!", __FUNCTION__, __LINE__);
					errorResponse(&(iter->fBase->header), &(iter->fBase->subHeader), (BindPtr*)pVS->pCurr, errFlag, errorStr);
				}
			}
			else
			{
				int errFlag =  (iter->fBase->tCmd == CMD_UPLOAD || iter->fBase->tCmd == CMD_UPLOAD4ALLINFO) ? 1 : 3;
				sprintf(errorStr, "%s: %d handle %d failed", __FUNCTION__, __LINE__, iter->fBase->tCmd);
				errorResponse(&(iter->fBase->header), &(iter->fBase->subHeader), (BindPtr*)pVS->pCurr, errFlag, errorStr);
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

bool  CWorkThread::parseUpload4AllInfo()
{
	g_log.Wlog(5, "%s\n", __FUNCTION__);
	char* pData = (char*)(m_pCurr->RBuf.Data());

	AllInfor4Upload*   ptrUpload = new AllInfor4Upload();
	ptrUpload->tCmd = CMD_UPLOAD4ALLINFO;

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
	if (!getNstring(pData, PackLen, ACC_MAXUNAME, &nPos, ptrUpload->name))
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
	}

	//----------------------passwd------------------------
	if (!getNstring(pData, PackLen, ACC_MAXUPASSWD, &nPos, ptrUpload->passwd))
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
	}

	if (0 != strlen(ptrUpload->name))
	{
		if ( strlen(ptrUpload->name) < 3
			|| !CUnit::isValid(std::string(ptrUpload->name))
			|| (!Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr)))
			&& (strlen(ptrUpload->passwd) < 3))
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
		}
	}

	//--------------------------------mac address------------------------------
	if (!getNstring(pData, PackLen, &nPos, ptrUpload->macIP) || !ptrUpload->macIP.length())
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
	}

	//-------------------------------push id----------------------------
	if (!getNstring(pData, PackLen, &nPos, ptrUpload->pushID.first))
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
	}

	if (!getuint8_t(pData, PackLen, &nPos, &(ptrUpload->pushID.second)))
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
	}

	//----------------------------client id------------------------------
	if (!getNstring(pData, PackLen, &nPos, ptrUpload->clientID))
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
	}

	//---------------------------phone number---------------------------
	if (!getNstring(pData, PackLen, &nPos, ptrUpload->phoneNum))
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
	}

	// ---------------------row num------------------------
	if (!getChar(pData, PackLen, &nPos, &(ptrUpload->row)) || ptrUpload->row <= 0 || ptrUpload->row > Config::instance().getMaxRow())
	{
		delete ptrUpload;
		return errorResponse(pHead, pSub_head, m_pCurr, 2, "CMD_UPLOAD4ALLINFO");
	}

	int SizeOfNstring = sizeof(nString);
	char tmpstock[1024] = {0};
	ptrUpload->dt.resize(ptrUpload->row);
	bool needExchange = false;
	for (int i = 0; i < ptrUpload->row; ++i)
	{
		//--------------sort--------------
		if (!getuint8_t(pData, PackLen, &nPos, &(ptrUpload->dt[i].sort)) || !ptrUpload->dt[i].sort)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
		}
		//---------------group-------------
		if (!getuint8_t(pData, PackLen, &nPos, &(ptrUpload->dt[i].group)) || !ptrUpload->dt[i].group)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
		}
		//---------------action-------------
		if (!getChar(pData, PackLen, &nPos, &(ptrUpload->dt[i].action))
			|| ptrUpload->dt[i].action < 0 || ptrUpload->dt[i].action > 2)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
		}
		//---------------stock num-------------
		short arrNum = 0;
		if (!getShort(pData, PackLen, &nPos, &arrNum) || arrNum < 0 || arrNum > 100)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD4ALLINFO");
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

	//if ( 0 != strlen(ptrUpload->name)
	//	&& !Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr))
	//	&& UserMgr::instance().CheckUser(ptrUpload->name, ptrUpload->passwd)
	//	&& !m_mongoSelf.checkUserPasswd(std::string(ptrUpload->name), std::string(ptrUpload->passwd)))
	//{
	//	// async send to AccCenter
	//	if (!m_ChkIsFirst.IsFirstOrTimeOuted(ptrUpload->name))
	//	{
	//		g_log.Wlog(1, "CWorkThread::parseUpload m_ChkIsFirst.IsFirstOrTimeOuted(%s) failed.....\n", ptrUpload->name);
	//		delete ptrUpload;
	//		return errorResponse(pHead, pSub_head, m_pCurr, 1, "CMD_UPLOAD");
	//	}

	//	VStruct* pVerify = new VStruct();
	//	pVerify->tAsyn = ASYNC_GET_USER_INFO;
	//	strcpy(pVerify->uname, ptrUpload->name);
	//	strcpy(pVerify->passwd, ptrUpload->passwd);
	//	pVerify->extra = pHead->m_nExpandInfo;
	//	pVerify->LoginTime = time(0);
	//	pVerify->bSend = 0;
	//	pVerify->pWorkThread = this;
	//	pVerify->pCurr = m_pCurr;

	//	Actor  pMsg;
	//	pMsg.fBase.reset(ptrUpload);
	//	pMsg.sBase.reset(pVerify);

	//	ThManage::instance().Post2AccCenter(pMsg);
	//	g_log.Wlog( 3, "CWorkThread::parseUpload into AccCenter.uname = %s\n",pVerify->uname);
	//}
	//else
	//{
		g_log.Wlog( 3, "CWorkThread::%s begin process CMD_UPLOAD4ALLINFO uname = %s\n", __FUNCTION__, ptrUpload->name);
		Cmd_Upload4AllInfo(ptrUpload, m_pCurr);
		delete ptrUpload;
	//}

	return true;
}

bool  CWorkThread::parseDownload4AllInfo()
{
	g_log.Wlog(5, "%s\n", __FUNCTION__);
	char* pData = (char*)(m_pCurr->RBuf.Data());

	AllInfo4Download*   ptrDownload = new AllInfo4Download();
	ptrDownload->tCmd = CMD_DOWNLOAD4ALLINFO;

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
	if (!getNstring(pData, PackLen, ACC_MAXUNAME, &nPos, ptrDownload->name))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD4ALLINFO");
	}

	//----------------------passwd------------------------
	if (!getNstring(pData, PackLen, ACC_MAXUPASSWD, &nPos, ptrDownload->passwd))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD4ALLINFO");
	}

	if (strlen(ptrDownload->name))
	{
		if (strlen(ptrDownload->name) < 3
			|| !CUnit::isValid(ptrDownload->name)
		    || !Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr))
		    && strlen(ptrDownload->passwd) < 3)
		{
			delete ptrDownload;
			return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD4ALLINFO");
		}
	}

	//--------------------------------mac address------------------------------
	if (!getNstring(pData, PackLen, &nPos, ptrDownload->macIP))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_UPLOAD4ALLINFO");
	}

	//-------------------------------push id----------------------------
	if (!getNstring(pData, PackLen, &nPos, ptrDownload->pushID.first))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_UPLOAD4ALLINFO");
	}

	if (!getuint8_t(pData, PackLen, &nPos, &(ptrDownload->pushID.second)))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_UPLOAD4ALLINFO");
	}

	if ( 0 == strlen(ptrDownload->name)
		 && 0 == ptrDownload->macIP.length())
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_UPLOAD4ALLINFO");
	}


	if (!getRowCacheHead(pData, PackLen, &nPos, &(ptrDownload->rCH)))
	{
		delete ptrDownload;
		return errorResponse(pHead, pSub_head, m_pCurr, 4, "CMD_DOWNLOAD");
	}


	//if ( strlen(ptrDownload->name)
	//	 && !Config::instance().isSafeIP(inet_ntoa(m_pCurr->addr.sin_addr))
	//	 && UserMgr::instance().CheckUser(ptrDownload->name, ptrDownload->passwd)
	//	 && !m_mongoSelf.checkUserPasswd(std::string(ptrDownload->name), std::string(ptrDownload->passwd)))
	//{
	//	// async send to AccCenter
	//	if (!m_ChkIsFirst.IsFirstOrTimeOuted(ptrDownload->name))
	//	{
	//		g_log.Wlog(1, "CWorkThread::parseDownload m_ChkIsFirst.IsFirstOrTimeOuted(%s) failed.....\n", ptrDownload->name);
	//		delete ptrDownload;
	//		return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_DOWNLOAD");
	//	}

	//	VStruct* pVerify = new VStruct();
	//	pVerify->tAsyn = ASYNC_GET_USER_INFO;
	//	strcpy(pVerify->uname, ptrDownload->name);
	//	strcpy(pVerify->passwd, ptrDownload->passwd);
	//	pVerify->extra = pHead->m_nExpandInfo;
	//	pVerify->LoginTime = time(0);
	//	pVerify->bSend = 0;
	//	pVerify->pWorkThread = this;
	//	pVerify->pCurr = m_pCurr;

	//	Actor pMsg;
	//	pMsg.fBase.reset(ptrDownload);
	//	pMsg.sBase.reset(pVerify);

	//	ThManage::instance().Post2AccCenter(pMsg);
	//	g_log.Wlog( 3, "CWorkThread::parseDownload into AccCenter.uname = %s\n",pVerify->uname);
	//}
	//else
	//{
		g_log.Wlog( 3, "CWorkThread::%s begin process CMD_DOWNLOAD4ALLINFO uname = %s\n", __FUNCTION__, ptrDownload->name);
		Cmd_Download4AllInfo(ptrDownload, m_pCurr);
		delete ptrDownload;
	//}

	return true;
}

bool  CWorkThread::parseUpload()
{
	//fprintf(stderr, "%s start time %lld\n", __FUNCTION__, CUnit::US());
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
	if (!getNstring(pData, PackLen, ACC_MAXUNAME, &nPos, ptrUpload->name) || strlen(ptrUpload->name) < 3 || !CUnit::isValid(ptrUpload->name))
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
		return errorResponse(pHead, pSub_head, m_pCurr, 2, "CMD_UPLOAD");
	}

	int SizeOfNstring = sizeof(nString);
	char tmpstock[1024] = {0};
	ptrUpload->dt.resize(ptrUpload->row);
	bool needExchange = false;
	for (int i = 0; i < ptrUpload->row; ++i)
	{
		//--------------sort--------------
		if (!getuint8_t(pData, PackLen, &nPos, &(ptrUpload->dt[i].sort)) || !ptrUpload->dt[i].sort)
		{
			delete ptrUpload;
			return errorResponse(pHead, pSub_head, m_pCurr, 3, "CMD_UPLOAD");
		}
		//---------------group-------------
		if (!getuint8_t(pData, PackLen, &nPos, &(ptrUpload->dt[i].group)) || !ptrUpload->dt[i].group)
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
			&& UserMgr::instance().CheckUser(ptrUpload->name, ptrUpload->passwd)
			&& !m_mongoSelf.checkUserPasswd(std::string(ptrUpload->name), std::string(ptrUpload->passwd)))
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
		g_log.Wlog( 3, "CWorkThread::parseUpload begin process CMD_UPLOAD uname = %s\n", ptrUpload->name);
		Cmd_Upload(ptrUpload, m_pCurr);
		delete ptrUpload;
	}

	return true;
}

bool  CWorkThread::parseDownload()
{
	//fprintf(stderr, "%s start time %lld\n", __FUNCTION__, CUnit::US());
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
	if (!getNstring(pData, PackLen, ACC_MAXUNAME, &nPos, ptrDownload->name) || strlen(ptrDownload->name) < 3 || !CUnit::isValid(ptrDownload->name))
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
			&& UserMgr::instance().CheckUser(ptrDownload->name, ptrDownload->passwd)
			&& !m_mongoSelf.checkUserPasswd(std::string(ptrDownload->name), std::string(ptrDownload->passwd)))
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
		g_log.Wlog( 3, "CWorkThread::parseDownload begin process CMD_DOWNLOAD uname = %s\n",ptrDownload->name);
		Cmd_Download(ptrDownload, m_pCurr);
		delete ptrDownload;
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

				case SYNC_UPLOAD4ALLINFO:
					parseUpload4AllInfo();
					break;

				case SYNC_DOWNLOAD4ALLINFO:
					parseDownload4AllInfo();
					break;

				case SYNC_GET_PHONE_NUMBER:
					Cmd_get_phone_number();
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

bool CWorkThread::getNstring(const char* src, const size_t PackLen, size_t* pos, std::string& des)
{
	if (!src || !pos)
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
		des.assign(pNStr->buf, pNStr->len);
	}

	*pos += pNStr->len;

	return true;
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

bool CWorkThread::getuint8_t(const char* src, const size_t PackLen, size_t* pos, uint8_t* des)
{
	if (!src || !pos || !des)
	{
		return false;
	}

	if (*pos + sizeof(uint8_t) > PackLen)
	{
		return false;
	}

	*des = *((uint8_t*)(src + *pos));
	*pos += sizeof(uint8_t);

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

bool CWorkThread::Cmd_get_phone_number()
{
	g_log.Wlog(5, "%s", __FUNCTION__);
	char* pData = (char*)(m_pCurr->RBuf.Data());

	ACC_CMDHEAD* pHead = (ACC_CMDHEAD*)(pData);
	sub_head* pSub_head = (sub_head*)( pHead + 1 );
	size_t PackLen = sizeof(ACC_CMDHEAD) + pHead->m_nLen;

	// ----------------------ACC_CMDHEAD--------------------
	size_t nPos = sizeof(ACC_CMDHEAD);

	//----------------------sub_head----------------------
	nPos += sizeof(sub_head);

	//---------------------mac address--------------------
	std::string macIP;
	if (!getNstring(pData, PackLen, &nPos, macIP) || !macIP.length())
	{
		return errorResponse(pHead, pSub_head, m_pCurr, 2, "CMD_Get_phone_number");
	}

	std::string phone;
	if (!m_mongoSelf.getPhoneNumByMacIP(macIP, phone) || !phone.length())
	{
		return errorResponse(pHead, pSub_head, m_pCurr, 1, "CMD_Get_phone_number");
	}

	CDBuffer  rBuf;
	rBuf.appendShort((short)(phone.length()));
	rBuf.append(phone.c_str(), phone.length());

	//返回数据
	sub_head rSub_head;
	memset(&rSub_head, 0, sizeof(sub_head));
	rSub_head.sub_type = pSub_head->sub_type;
	rSub_head.sub_length = sizeof(char) +  sizeof(nString) + phone.length();

	ACC_CMDHEAD rHead;
	memset(&rHead, 0, sizeof(ACC_CMDHEAD));
	rHead.m_wCmdType = pHead->m_wCmdType;
	rHead.m_nExpandInfo = pHead->m_nExpandInfo;
	rHead.m_nLen = rSub_head.sub_length + sizeof(sub_head);

	m_pCurr->SBuf.append(&rHead, sizeof(ACC_CMDHEAD));
	m_pCurr->SBuf.append(&rSub_head, sizeof(sub_head));
	m_pCurr->SBuf.appendChar(0);
	m_pCurr->SBuf.append(rBuf.Data(),rBuf.Size());

	struct epoll_event ev;
	ev.data.ptr = m_pCurr;
	ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
	epoll_ctl(m_epfd, EPOLL_CTL_MOD, m_pCurr->fd, &ev);

	g_log.Wlog(3, "%s: CMD_Get_phone_number stock !!!\n", __FUNCTION__);

	return true;
}

bool CWorkThread::Cmd_Download4AllInfo(FoundBase* pCmd, BindPtr* pBind)
{
	g_log.Wlog(3, "%s: %d starting!!!\n", __FUNCTION__, __LINE__);
	if (!pCmd || !pBind)
	{
		return false;
	}

	AllInfo4Download*  pDownload = dynamic_cast<AllInfo4Download*>(pCmd);
	if (!pDownload)
	{
		return false;
	}

	if (!strlen(pDownload->name) && pDownload->macIP.length())
	{
		std::string uname;
		if (m_mongoSelf.getUserAccount(Config::instance().getStockDataDB(), pDownload->macIP, uname)
			|| m_mongoSelf.getUserAccount(Config::instance().getSyncronizeDB(), pDownload->macIP, uname))
		{
			strcpy(pDownload->name, uname.c_str());
		}
	}

	CDBuffer  rBuf;
	char  rRetNum = 0;
	char rRet = 0;

	char errorStr[1024] = { 0 };

	if (pDownload->rCH.sort == 0 && pDownload->rCH.group == 0)
	{
		std::vector<AtomRecordDB>  vRes;
		//queryAllStockData4User(*pDownload, vRes);
		m_mongoSelf.queryAllStockData(pDownload->name, pDownload->macIP, Config::instance().getStockDataDB(), vRes);
		m_mongoSelf.queryAllStockData(pDownload->name, pDownload->macIP, Config::instance().getSyncronizeDB(), vRes);
		std::vector<AtomRecordDB>::iterator iter = vRes.begin();
		while (iter != vRes.end())
		{
			short shortlen = 0;
			uint8_t st = (uint8_t)(iter->sort);
			uint8_t gp = (uint8_t)(iter->group);
			rBuf.append(&st, sizeof(uint8_t));
			rBuf.append(&gp, sizeof(uint8_t));
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

		if (vRes.size() <= 0)
		{
			rRet = 2;
		}
	}
	else
	{
		bool executeflag = false;
		short shortlen = 0;
		AtomRecordDB  atomData;
		atomData.uname = pDownload->name;
		atomData.macIP = pDownload->macIP;
		atomData.pushID = pDownload->pushID;
		atomData.sort = pDownload->rCH.sort;
		atomData.group = pDownload->rCH.group;
		if (1 == atomData.sort && 1 == atomData.group)
		{
			if (userDataExistInCloudDB(atomData))
			{
				executeflag = true;
				StkTypeMgr::instance().removeSuffix(atomData.content);
			}
		}
		else
		{
			executeflag = getCombinationData(atomData);
		}

		if (executeflag)
		{
			if (atomData.ver > pDownload->rCH.ver)
			{
				uint8_t st = (uint8_t)(atomData.sort);
				uint8_t gp = (uint8_t)(atomData.group);
				rBuf.append(&st, sizeof(uint8_t));
				rBuf.append(&gp, sizeof(uint8_t));
				rBuf.appendInt64(atomData.ver);
				rBuf.appendShort(shortlen);

				shortlen = atomData.content.size();
				rBuf.appendShort(shortlen);

				CUnit::getStockData(atomData.content, rBuf);

				++rRetNum;
			}
			else
			{
				rRet = 1;
			}
		}
		else
		{
			rRet = 2;
		}
	}

	if (rRet)
	{
		sprintf(errorStr, "%s: %d handle CMD_DOWNLOAD %d ",  __FUNCTION__, __LINE__, pCmd->tCmd);
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

bool CWorkThread::mergeStock(const std::vector<AtomRecordDB>& vResource, std::vector<AtomRecordDB>& vResult)
{
	if (vResource.size() <= 0)
	{
		return false;
	}

	size_t tz = vResource.size();
	std::vector<UniqueIdentfy4Set> vSet;
	for (size_t i = 0; i < tz; ++i)
	{
		std::vector<UniqueIdentfy4Set>::iterator it = std::find(vSet.begin(), vSet.end(), UniqueIdentfy4Set(vResource[i].sort, vResource[i].group));
		if ( it != vSet.end())
		{
			it->vec.push_back(i);
		}
		else
		{
			UniqueIdentfy4Set  ele(vResource[i].sort, vResource[i].group);
			ele.vec.push_back(i);
			vSet.push_back(ele);
		}
	}

	// filter the duplicate
	std::vector<UniqueIdentfy4Set>::iterator iter;
	for (iter = vSet.begin(); iter != vSet.end(); ++iter)
	{
		for (size_t j = 0; j < iter->vec.size(); ++j)
		{
			if (iter->vec[j] == -1)
			{
				continue;
			}

			for (size_t k = j + 1; k < iter->vec.size(); ++k)
			{
				if (iter->vec[k] == -1)
				{
					continue;
				}

				if (vResource[iter->vec[k]].content == vResource[iter->vec[j]].content)
				{
					iter->vec[k] = -1;
				}
			}
		}
	}

	// merge start
	for (iter = vSet.begin(); iter != vSet.end(); ++iter)
	{
		AtomRecordDB  rec;
		rec.sort = iter->sort;
		rec.group = iter->group;

		std::multimap<uint64_t, size_t>    mulmap;
		for (size_t i = 0; i < iter->vec.size(); ++i)
		{
			if (iter->vec[i] != -1)
			mulmap.insert(std::make_pair(vResource[iter->vec[i]].ver, iter->vec[i]));
		}

		std::multimap<uint64_t, size_t>::iterator itermulmap = mulmap.begin();
		while (itermulmap != mulmap.end())
		{
			std::vector<_SymbolStock>::const_iterator itStkSrc = vResource[itermulmap->second].content.begin();
			while (itStkSrc != vResource[itermulmap->second].content.end())
			{
				if (std::find(rec.content.begin(), rec.content.end(), *itStkSrc) == rec.content.end())
				{
					rec.content.push_back(*itStkSrc);
				}
				++itStkSrc;
			}

			++itermulmap;
		}

		vResult.push_back(rec);
	}

	return true;
}

char  CWorkThread::updateContent(const AllInfor4Upload& info, CDBuffer& rBuf, char& rRetNum)
{
	std::set<std::string>  vDev;
	vDev.insert(info.macIP);
	if (strlen(info.name))
	{
		vDev.insert(std::string(""));
		m_mongoSelf.theNumberOfDevOnAccount(std::string(info.name), vDev);
	}

	char ret = 0;
	for (int i = 0; i < info.row; ++i)
	{
		RowCacheHead RCH;
		RCH.sort = info.dt[i].sort;
		RCH.group = info.dt[i].group;
		RCH.ver = 0;

		AtomRecordDB  atomData;
		atomData.uname = info.name;
		atomData.macIP = info.macIP;
		atomData.sort = info.dt[i].sort;
		atomData.group = info.dt[i].group;

		int isChange = 0;
		if (0 == info.dt[i].action)
		{
			queryStockData4Upload(atomData);
			addChangeData2Set(atomData, info.dt[i].stock, isChange);
		}
		else if (1 == info.dt[i].action)
		{
			queryStockData4Upload(atomData);
			addDelData2Set(atomData, info.dt[i].stock, isChange);
		}
		else if ( 2 == info.dt[i].action)
		{
			isChange = 1;
			appendChangeData2Set(atomData, info.dt[i].stock);

		}
		else
		{
			ret = 3;
			g_log.Wlog(1, "invalid action: %d!!!\n", info.dt[i].action);
			break;
		}

		if (isChange)
		{
			bool succFlag = true;
			atomData.ver = CUnit::US();
			std::set<std::string>::const_iterator itDev =  vDev.begin();
			while (itDev != vDev.end())
			{
				atomData.macIP = *itDev;
				succFlag = succFlag && setStkOfRecInDB(atomData);

				if (!succFlag) break;
				++itDev;
			}

			if (!succFlag)
			{
				ret = 3;
				g_log.Wlog(1, "add update Stock to mongodb failed!!!\n");
				break;
			}
		}

		rBuf.append(&RCH, sizeof(RCH));
		++rRetNum;
	}

	return ret;
}

bool CWorkThread::Cmd_Upload4AllInfo(FoundBase*  pCmd, BindPtr* pBind)
{
	g_log.Wlog(3, "%s: %d starting!!!\n", __FUNCTION__, __LINE__);
	if (!pCmd || !pBind)
	{
		return false;
	}

	AllInfor4Upload*  pUpload = dynamic_cast<AllInfor4Upload*>(pCmd);
	if (!pUpload)
	{
		return false;
	}

	char errorStr[2048] = { 0 };

	// for response flag
	char ret = 0;
	char rRetNum = 0;
	CDBuffer rBuf;

	if (strlen(pUpload->name))  //1, update with name
	{
		std::string name;
		if (!m_mongoSelf.getUserAccount(Config::instance().getStockDataDB(), pUpload->macIP, name))
		{
			m_mongoSelf.getUserAccount(Config::instance().getSyncronizeDB(), pUpload->macIP, name);
		}

		if (name.length()) //(1)name exist already
		{
			if (!strcmp(pUpload->name, name.c_str()))
			{
				ret = updateContent(*pUpload, rBuf, rRetNum);
				if (!ret)
				{
					if (!m_mongoSelf.updateAccInfo(*pUpload))
					{
						ret = 3;
						g_log.Wlog(1, "%s update user information failed!!!\n", __FUNCTION__);
					}
				}
			}
			else
			{
				std::vector<AtomRecordDB> vResource;
				m_mongoSelf.queryAllStockData(name, pUpload->macIP, Config::instance().getStockDataDB(), vResource);
				m_mongoSelf.queryAllStockData(name, pUpload->macIP, Config::instance().getSyncronizeDB(), vResource);
				queryAllStockData4User(std::string(pUpload->name), vResource);

				// unbind name and device id in private db
				if (m_mongoSelf.isCombinationExist(name, std::string()))
				{
					m_mongoSelf.removeExactlyUserData(name, pUpload->macIP);
				}
				else
				{
					m_mongoSelf.resetUserMacIP(name, std::string());
				}

				// unbind name and device id in share db(share and private db is difference)
				m_mongoSelf.unbindInShareDB(name, pUpload->macIP);

				std::vector<AtomRecordDB> vResult;
				if (mergeStock(vResource, vResult))
				{
					// update all device of using the same name
					std::set<std::string>  vDev;
					vDev.insert(pUpload->macIP);
					vDev.insert(std::string(""));
					m_mongoSelf.theNumberOfDevOnAccount(std::string(pUpload->name), vDev);

					std::vector<AtomRecordDB>::iterator itVec;
					std::set<std::string>::iterator itDev;
					for (itDev = vDev.begin(); itDev != vDev.end(); ++itDev)
					{
						for (itVec = vResult.begin(); itVec != vResult.end(); ++itVec)
						{
							itVec->uname.assign(pUpload->name);
							itVec->macIP = *itDev;
							itVec->ver = CUnit::US();
							setStkOfRecInDB(*itVec);
						}
					}
				}

				ret = updateContent(*pUpload, rBuf, rRetNum);
				if (!ret)
				{
					if (!m_mongoSelf.updateAccInfo(*pUpload))
					{
						ret = 3;
						g_log.Wlog(1, "%s update user information failed!!!\n", __FUNCTION__);
					}
				}
			}
		}
		else //(2) name not exist
		{
			// merge stock only
			std::vector<AtomRecordDB> vResource;
			m_mongoSelf.queryAllStockData(name, pUpload->macIP, Config::instance().getStockDataDB(), vResource);
			m_mongoSelf.queryAllStockData(name, pUpload->macIP, Config::instance().getSyncronizeDB(), vResource);
			queryAllStockData4User(std::string(pUpload->name), vResource);

			std::vector<AtomRecordDB> vResult;
			if (mergeStock(vResource, vResult))
			{
				// get user info
				AtomRecordDB  aInfo;
				CloudRecoreDB  cInfo;
				aInfo.macIP = cInfo.macIP = pUpload->macIP;
				m_mongoSelf.getAccInfo(aInfo);
				m_mongoSelf.getAccInfo(cInfo);

				//delete record
				m_mongoSelf.removeExactlyUserData(name, pUpload->macIP);

				// update all device of using the same name
				std::set<std::string>  vDev;
				vDev.insert(pUpload->macIP);
				vDev.insert(std::string(""));
				m_mongoSelf.theNumberOfDevOnAccount(std::string(pUpload->name), vDev);

				std::vector<AtomRecordDB>::iterator itVec;
				std::set<std::string>::iterator itDev;
				for (itDev = vDev.begin(); itDev != vDev.end(); ++itDev)
				{
					for (itVec = vResult.begin(); itVec != vResult.end(); ++itVec)
					{
						itVec->uname.assign(pUpload->name);
						itVec->macIP = *itDev;
						itVec->ver = CUnit::US();
						setStkOfRecInDB(*itVec);
					}
				}

				// update user info
				aInfo.uname.assign(pUpload->name);
				cInfo.userid.assign(pUpload->name);

				if (pUpload->pushID.first.length())
				{
					aInfo.pushID = cInfo.pushID = pUpload->pushID;
				}

				if (pUpload->phoneNum.length())
				{
					aInfo.phoneNum = cInfo.phoneNum = pUpload->phoneNum;
				}

				m_mongoSelf.updateAccInfo(aInfo);
				m_mongoSelf.updateAccInfo(cInfo);
			}

			ret = updateContent(*pUpload, rBuf, rRetNum);
		}

		m_mongoSelf.updateExternalInfoInAccUserCache(*pUpload);
	}
	else //2, update without name
	{
		std::string name;
		if (m_mongoSelf.getUserAccount(Config::instance().getStockDataDB(), pUpload->macIP, name)
				|| m_mongoSelf.getUserAccount(Config::instance().getSyncronizeDB(), pUpload->macIP, name))
		{
			if (name.length() > 0)
				strncpy(pUpload->name, name.c_str(), (name.length() > ACC_MAXUNAME ? ACC_MAXUNAME : name.length()));
		}

		ret = updateContent(*pUpload, rBuf, rRetNum);
		if (!ret)
		{
			if (!m_mongoSelf.updateAccInfo(*pUpload))
			{
				ret = 3;
				g_log.Wlog(1, "%s update user information failed!!!\n", __FUNCTION__);
			}

			m_mongoSelf.updateExternalInfoInAccUserCache(*pUpload);
		}
	}

	if ( ret )
	{
		sprintf(errorStr, "error happened in CMD_UPLOAD4ALLINFO, ret = %d\n", ret);
		return errorResponse(&(pUpload->header), &(pUpload->subHeader), pBind, ret, errorStr);
	}

	//返回数据
	sub_head rSub_head;
	memset(&rSub_head, 0, sizeof(sub_head));
	rSub_head.sub_type = pUpload->subHeader.sub_type;
	rSub_head.sub_length = sizeof(char) + sizeof(char) + rBuf.Size();

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

	g_log.Wlog(3, "%s: CMD_UPLOAD4ALLINFO stock success!!!\n", __FUNCTION__);

	return true;
}

