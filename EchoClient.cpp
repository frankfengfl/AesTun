// EchoClient.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "global.h"
#include "globalEpoll.h"
#undef BUFFER_SIZE

std::string ip = "127.0.0.1";
int nPort = 12345;

static SOCKET sClient;
#define CLENT_NUM 1
static SOCKET sSever, sSever_c[CLENT_NUM];
struct sockaddr_in sSever_c_sd[CLENT_NUM];

#define MAX_SEND_SIZE   512
#define BUFFER_SIZE     MAX_SEND_SIZE+1

#define USER_ERROR -1

#ifdef _WIN32
#define LFRP_SEND_FLAGS     0
#else
#define LFRP_SEND_FLAGS     MSG_NOSIGNAL
#endif

#include<sys/time.h>
#include <atomic>

#define TEST_LOOP_COUNT 500
std::thread** pEchoCliThreadAry = nullptr;
struct CStringPair
{
    std::string sSend;
    std::string sRecv;
};

typedef std::map<int, CStringPair> CSocketDataMap;
CSocketDataMap* pSockDataMapAry = nullptr;
std::atomic<int> nCorrectCount(0);
std::atomic<int> nConnectCount(0);
std::atomic<int> nCloseCount(0);
struct timeval tmStart;

// 全局sock对实例的映射字典
std::shared_mutex mtxEchoSockToInstance;
std::map<int, CLfrpSocket*> mapEchoSockToClass;
void EchoAddSockToInstanceMap(int sock, CLfrpSocket* pSocket)
{
    CWriteLock lock(mtxEchoSockToInstance);
    std::map<int, CLfrpSocket*>::iterator iter = mapEchoSockToClass.find(sock);
    if (iter != mapEchoSockToClass.end())
    {
        mapEchoSockToClass.erase(iter);;
    }
    mapEchoSockToClass.insert(std::make_pair(sock, pSocket));
}

void EchoRemoveSockFromInstanceMap(int sock)
{
    CWriteLock lock(mtxEchoSockToInstance);
    std::map<int, CLfrpSocket*>::iterator iter = mapEchoSockToClass.find(sock);
    if (iter != mapEchoSockToClass.end())
    {
        mapEchoSockToClass.erase(iter);;
    }
}

void ExitProcess()
{
    ExitWorkerThreads();

    struct timeval tmEnd;
    gettimeofday(&tmEnd, NULL);
    int nDiffTime = 1000* (tmEnd.tv_sec - tmStart.tv_sec) + (tmEnd.tv_usec - tmStart.tv_usec) / 1000;

    int nCoCnt = nCorrectCount;
    int nClCnt = nCloseCount;
    double nCountPerSec = nThreadCount * TEST_LOOP_COUNT / ((double)nDiffTime / 1000);
    PRINT_ERROR("%s %s,%d: test finish take %dms with %d correct in %d test count, TPC %f\n", GetCurTimeStr(), __FUNCTION__, __LINE__, nDiffTime, nCoCnt, nClCnt, nCountPerSec);
#ifdef LOG_TO_FILE
    printf("%s %s,%d: test finish take %dms with %d correct in %d test count, TPC %f\n", GetCurTimeStr(), __FUNCTION__, __LINE__, nDiffTime, nCoCnt, nClCnt, nCountPerSec);
#endif

    // 打印失败内容：比如单机测试EchoServer抢不到资源，来不及accept；
    for (size_t i = 0; i < nThreadCount; i++)
    {
        for (CSocketDataMap::iterator iter = pSockDataMapAry[i].begin(); iter != pSockDataMapAry[i].end(); iter++)
        {
            //if (iter->second.length() == 0)
            {
                PRINT_ERROR("%s %s,%d: SocketID %d with data size %d didn't recv data\n", GetCurTimeStr(), __FUNCTION__, __LINE__, iter->first, iter->second.sSend.length());
            }
        }
    }

    for (std::map<int, CLfrpSocket*>::iterator iter = mapEchoSockToClass.begin(); iter != mapEchoSockToClass.end(); iter++)
    {
        PRINT_ERROR("%s %s,%d: SocketID %d didn't close\n", GetCurTimeStr(), __FUNCTION__, __LINE__, iter->first);
    }

    // todo，完整清理并退出
    exit(0);
}

int EchoCliTrans(int nIndex, int sock, CLfrpSocket* pSocket)
{
    return 0;
}

int EchoCliClose(int nIndex, int sock)
{
    CLfrpSocket* pSocket = GetSockFromInstanceMap(sock);
    if (!pSocket)
    { // 已经关闭不再尝试发送
        return 0;
    }

    PRINT_INFO("%s %s,%d: thread %d SocketID %d close\n", GetCurTimeStr(), __FUNCTION__, __LINE__, nIndex, sock);
    //close(sock);
    //AddDelayClose(sock);
    CloseSocketInstance(nIndex, sock);
    EchoRemoveSockFromInstanceMap(sock);

    nCloseCount++;

    if (nCloseCount == TEST_LOOP_COUNT * nThreadCount)
    {
        ExitProcess();
    }

    return 0;
}

int EchoCliRead(int nIndex, int sock, char* pBuffer, int nCount)
{
    CSocketDataMap::iterator iter = pSockDataMapAry[nIndex].find(sock);
    if (iter != pSockDataMapAry[nIndex].end())
    {
        std::string& strSend = iter->second.sSend;
        std::string& strRecv = iter->second.sRecv;
        std::string strTmp(pBuffer, nCount);
        strRecv += strTmp;
        if (strSend.length() > strRecv.length())
        {
            PRINT_INFO("%s %s,%d: SocketID %d recv part pack\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock);
        }
        else if (strSend.compare(strRecv) != 0)
        {
            PRINT_ERROR("%s %s,%d: SocketID %d with %d:%s recv error data %d:%s\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, strSend.length(), strSend.c_str(), strRecv.length(), strRecv.c_str());
            pSockDataMapAry[nIndex].erase(iter);
            EchoCliClose(nIndex, sock);     // Fire关闭会导致事件中间再Write数据
        }
        else
        {
            nCorrectCount++;
            PRINT_INFO("%s %s,%d: OK: SocketID %d recv: %d:%s\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, strSend.length(), strSend.c_str());
            pSockDataMapAry[nIndex].erase(iter);
            EchoCliClose(nIndex, sock);     // Fire关闭会导致事件中间再Write数据
            //str.clear();
        }
    }
    else
    {
        PRINT_ERROR("%s %s,%d: cant find SocketID %d \n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock);
    }

    // 比较完关闭socket
    //FireCloseEvent(sock);
    //EchoCliClose(nIndex, sock);     // Fire关闭会导致事件中间再Write数据

    return 0;
}

int EchoCliWrite(int nIndex, int sock)
{
    if (pSockDataMapAry[nIndex].find(sock) != pSockDataMapAry[nIndex].end())
    { // 发送过，不再发
        return 0;
    }

    CLfrpSocket* pSocket = GetSockFromInstanceMap(sock);
    if (!pSocket)
    { // 已经关闭不再尝试发送
        return 0;
    }
    
    char cBuf[100] = { '0' };
    sprintf(cBuf, "%d,%d:", nIndex, sock);
    std::string str = cBuf; // "test";
    int nSize = rand() % MAX_SEND_SIZE;
    //nSize = 10*1024*1024; //10*1024*1024;
    int nBucket = (nSize + 25) / 26;
    for (size_t i = 0; i < nSize; i++)
    {
        char c = (rand() % 26) + 'a';
        //char c = (i % 26) + 'a';
        //char c = (i / nBucket)  + 'a';
        str += c;
    }
    CStringPair cPair;
    cPair.sSend = str;
    pSockDataMapAry[nIndex].insert(std::make_pair(sock, cPair));
    const char* pData = str.c_str();
    int nDataLen = str.length();
    while (nDataLen > 0)
    {
        int nSendSize = nDataLen > 1024 ? 1024 : nDataLen;
        int nRet = send(sock, pData, nSendSize, LFRP_SEND_FLAGS);
        while (nRet == SOCKET_ERROR && IsReSendSocketError(WSAGetLastError()))
        { // 堵住就等下一个EPOLLOUT事件，清掉已经发送的数据
            PRINT_ERROR("%s %s,%d: send to Tun err size %d wsaerr %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, str.length(), WSAGetLastError());
            //FireWriteEvent(sock);
            nRet = send(sock, pData, nSendSize, LFRP_SEND_FLAGS);
        }
        if (nRet == SOCKET_ERROR || nRet == 0)
        {
            PRINT_ERROR("%s %s,%d: SocketID %d disconnect because send err %x\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, nRet);
        }
        else
        {
            PRINT_INFO("%s %s,%d: SocketID %d send data size %d err %x\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, nSendSize, nRet);
            //PRINT_INFO("%s %s,%d: SocketID %d send data size %d:%s err %x\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, str.length(), str.c_str(), nRet);
        }
        pData += nSendSize;
        nDataLen -= nSendSize;
    }
    PRINT_INFO("%s %s,%d: SocketID %d send data size %d:%s\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, str.length(), str.c_str());
    
    return 0;
}

void EchoConnectWorker(int nIndex, const char* pIPAddr, int nPort)
{
    for (size_t i = 0; i < TEST_LOOP_COUNT; i++)
    {
        if (bExitPorcess)
        {
            return;
        }

        // 等待连接太多需要等一下
        if (nConnectCount - nCloseCount > 100)
        {
            usleep(1 * 1000);
        }

        int sock = INVALID_SOCKET;
        //EpollConnectSocket(epollfd, sock, pIPAddr, nPort);
        //FireTransEvent(sock);
        nConnectCount++;
        CLfrpSocket* pSocket = new CLfrpSocket;
        if (pSocket)
        {
            if (PreConnectSocket(pSocket->sock, pIPAddr, nPort) != 0)
            {
                pSocket->sock = INVALID_SOCKET;
                PRINT_ERROR("%s connect() Tun Failed: %d\n", GetCurTimeStr(), WSAGetLastError());
            }
            else
            {
                PRINT_INFO("%s %s,%d: EchoConnectWorker connect SocketID %d \n", GetCurTimeStr(), __FUNCTION__, __LINE__, pSocket->sock);
                // 先把sock放到全局环境
                sock = pSocket->sock;
                pSocket->nServiceNumber = sock;         // 测试用例直接按socket分配简单点
                AddSockToInstanceMap(sock, pSocket);    // 添加实例，用于判断sock是否可用
                EchoAddSockToInstanceMap(sock, pSocket);

                // 实际连接
                int nRet = ProcssConnectSocket(pSocket->sock, pIPAddr, nPort);
                nRet = EpollPostConnectSocket(epollfd, pSocket->sock, pIPAddr, nPort, nRet);
                if (nRet < 0)
                {
                    PRINT_ERROR("%s %s,%d: EpollPostConnectSocket error: %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, nRet);
                    CloseLfrpSocket(pSocket);
                    delete pSocket;
                    sock = INVALID_SOCKET;
                }
                else
                {
                    //FireTransEvent(sock);                   // 等待可写后，避免可写先完成，这里发个事件
                }
            }
        }
        else
        {
            PRINT_ERROR("%s %s,%d: thread %d index %d new CLfrpSocket error\n", GetCurTimeStr(), __FUNCTION__, __LINE__, nIndex, i);
        }
    }
}

int EchoCliTimer()
{
    CommTimer();

    static bool bExit = false;
    if (!bExit && !bExitPorcess)
    {
        struct timeval tmEnd;
        gettimeofday(&tmEnd, NULL);
        int nDiffTime = 1000 * (tmEnd.tv_sec - tmStart.tv_sec) + (tmEnd.tv_usec - tmStart.tv_usec) / 1000;
        int nEndTime = nThreadCount * TEST_LOOP_COUNT * 1000 / 2 / 4;
        nEndTime = nEndTime > 5 ? nEndTime / 2 : nEndTime;
        if (nDiffTime > 30 * 1000/*nEndTime*/)  // todo, 超过时间根据机器情况设置
        {
            bExit = true;
            ExitProcess();
        }
    }
    return 0;
}

int mainEchoEpoll()
{
    // 初始化socket
    InitSocket();

    InitLog("./EchoCli.txt");

    // 初始化epooll工作线程相关
    SetBusWorkerCallBack(EchoCliRead, EchoCliWrite, EchoCliClose, EchoCliTrans, nullptr, EchoCliTimer);
    InitWorkerThreads();
    pSockDataMapAry = new CSocketDataMap[nThreadCount];
    pEchoCliThreadAry = new std::thread * [nThreadCount];

    gettimeofday(&tmStart, NULL);

    //nThreadCount = 1;
    for (int i = 0; i < nThreadCount; i++)
    {
        pEchoCliThreadAry[i] = new std::thread(EchoConnectWorker, i, ip.c_str(), nPort);
    }
    //EchoConnectWorker(0, ip.c_str(), nPort);

    SOCKET sockListen = INVALID_SOCKET;
    mainEpoll(epollfd, sockListen);
    bExitPorcess = true;

    return 0;
}

int main(int argc, char* argv[])
{
    int i = 0;
    for (i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 && i + 1 <= argc)
        {
            i++;
            ip = argv[i];
        }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 <= argc)
        {
            i++;
            nPort = atoi(argv[i]);
        }
    }
    
    mainEchoEpoll();
    return 0;
}
