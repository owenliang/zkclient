/*
 * distribute_lock.cc
 *
 *  Created on: 2015年7月24日
 *      Author: Administrator
 */

/**
 *  实现分布式锁服务，抢占模式（非自增节点排队模式）
 *
 *  流程：wget路径不存在或者被删除则创建路径，若创建失败则重走流程。
 *
 *  注：创建失败可能是网络中断导致，其实在zookeeper已经完成了创建，这样在wget的时候
 *  必须要判断当前的锁是自己创建的，并先删掉它，然后重走整个流程。
 *  （这里没有实现成判断锁是自己创建的就唤醒condition的原因，是因为watch还在生效，这对于后续
 *  逻辑全都会影响，所以通过一次删除操作将流程重置回去）
 */


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string>
#include "zkclient.h"

struct DistributeMutex {
	bool locked;
	pthread_cond_t lock_cond;
	pthread_mutex_t lock_mutex;
	pthread_mutex_t local_mutex;
	std::string id;
};

void LockGetNodeHandler(ZKErrorCode errcode, const std::string& path, const char* value, int value_len, void* context);

void LockNodeCreateHandler(ZKErrorCode errcode, const std::string& path, const std::string& value, void* context) {
	DistributeMutex* mutex = (DistributeMutex*)context;

	ZKClient& zkclient = ZKClient::GetInstance();

	if (errcode == kZKSucceed) {
		pthread_mutex_lock(&mutex->lock_mutex);
		mutex->locked = true;
		pthread_cond_signal(&mutex->lock_cond);
		pthread_mutex_unlock(&mutex->lock_mutex);
		printf("lock got!!! i'm winner~\n");
	} else if (errcode == kZKError) {
		zkclient.Create(path.c_str(), mutex->id, ZOO_EPHEMERAL, LockNodeCreateHandler, mutex);
	} else if (errcode == kZKNotExist) {
		assert(false); // 要上锁的路径父目录不存在，需要用户提前创建
	} else if (errcode == kZKExisted) {
		zkclient.GetNode(path, LockGetNodeHandler, mutex, true);
	}
}

void LockDeleteHandler(ZKErrorCode errcode, const std::string& path, void* context) {
	ZKClient& zkclient = ZKClient::GetInstance();

	if (errcode == kZKError) {
		zkclient.Delete(path, LockDeleteHandler, context);
	} else if (errcode == kZKSucceed) {
		// 成功删除legacy的lock，会触发LockGetNodeHandler收到deleted事件
	} else {
		// kZKNotExist 和 kZKNotEmpty 都属于非预期情况，需用户保证不对路径做外部操作。
		assert(false);
	}
}

void LockGetNodeHandler(ZKErrorCode errcode, const std::string& path, const char* value, int value_len, void* context) {
	DistributeMutex* mutex = (DistributeMutex*)context;

	ZKClient& zkclient = ZKClient::GetInstance();

	if (errcode == kZKSucceed) {
		// 已被锁，可以看一下是不是被本进程锁定（来源于本进程之前一次成功create但没有收到response的操作）
		if (mutex->id.compare(0, value_len, value) == 0) {
			// 遭遇到一个非预期create出来的lock，
			// 虽然表明已经被自己锁住，但是由于watch仍旧生效，会导致后续流程和生命期判定太复杂，所以
			// 先delete掉当前的lock，重走抢锁流程。
			printf("legacy lock found!!! delete it first.\n");
			zkclient.Delete(path, LockDeleteHandler, mutex);
		} else {
			// 非本进程锁定，那么等待它被释放（deleted事件)
		}
	} else if (errcode == kZKNotExist) {
		// 未锁，试图创建
		zkclient.Create(path.c_str(), mutex->id, ZOO_EPHEMERAL, LockNodeCreateHandler, mutex);
	} else if (errcode == kZKDeleted) {
		// 其他进程释放了锁（可能是自己create重复创建出来的锁)
		printf("lock release, create it~~~\n");
		zkclient.Create(path.c_str(), mutex->id, ZOO_EPHEMERAL, LockNodeCreateHandler, mutex);
	} else { // 错误重试
		zkclient.GetNode(path, LockGetNodeHandler, mutex, true);
	}
}

void DistributeMutexInit(DistributeMutex* mutex) {
	mutex->locked = false;
	pthread_cond_init(&mutex->lock_cond, NULL);
	pthread_mutex_init(&mutex->lock_mutex, NULL);
	pthread_mutex_init(&mutex->local_mutex, NULL);

	char hostname[1024];
	assert(gethostname(hostname, sizeof(hostname)) == 0);
	hostname[1023] = '\0';

	struct timeval tv;
	gettimeofday(&tv, NULL);

	char pid_ms[128];
	snprintf(pid_ms, sizeof(pid_ms), "%d-%ld", getpid(), tv.tv_sec * 1000000 + tv.tv_usec);
	//  分布式系统唯一标识一个进程：机器名+进程号+时间戳
	mutex->id.assign(hostname).append("-").append(pid_ms);
}

void DistributeMutexDestroy(DistributeMutex* mutex) {
	pthread_mutex_destroy(&mutex->local_mutex);
	pthread_mutex_destroy(&mutex->lock_mutex);
	pthread_cond_destroy(&mutex->lock_cond);
}

void DistributeLock(const std::string& path, DistributeMutex* mutex) {
	pthread_mutex_lock(&mutex->local_mutex);

	ZKClient& zkclient = ZKClient::GetInstance();
	zkclient.GetNode(path, LockGetNodeHandler, mutex, true);

	pthread_mutex_lock(&mutex->lock_mutex);
	while (!mutex->locked) {
		pthread_cond_wait(&mutex->lock_cond, &mutex->lock_mutex);
	}
	pthread_mutex_unlock(&mutex->lock_mutex);
}

void DistributeUnlock(const std::string& path, DistributeMutex* mutex) {
	ZKClient& zkclient = ZKClient::GetInstance();

	ZKErrorCode errcode;
	while ((errcode = zkclient.Delete(path)) == kZKError);

	assert(errcode == kZKSucceed);
	pthread_mutex_unlock(&mutex->local_mutex);
}

int main(int argc, char** argv) {
	ZKClient& zkclient = ZKClient::GetInstance();
	if (!zkclient.Init("127.0.0.1:3000,127.0.0.1:3001,127.0.0.1:3002", 10000, NULL, NULL, true, "./distribute_lock.log")) {
		fprintf(stderr, "ZKClient failed to init...\n");
		return -1;
	}

	DistributeMutex mutex;
	DistributeMutexInit(&mutex);

	DistributeLock("/lock", &mutex);
	printf("After Lock\n");
	sleep(5);
	DistributeUnlock("/lock", &mutex);
	printf("After UnLock\n");

	DistributeMutexDestroy(&mutex);

	return 0;
}
