#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include  <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netdb.h>
//#include <cJSON.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

void usage(int arg,char *argv)
{
	if(arg != 2)
	{
		printf("Usage: %s URL\n",argv);
		exit(0);
	}
}

bool check_response(char *httphead)
{
	char *tmp = calloc(1,128);
	memcpy(tmp, httphead, strstr(httphead, "\r\n")-httphead);

	if(strstr(tmp, "20") && !strstr(tmp, "206"))
	{
		fprintf(stderr, "对端服务器不支持断点续传，");
		fprintf(stderr, "本文件将重新下载... ...\n");
		return true;
	}
	else if(strstr(tmp, "206"))
	{
		return true;
	}

	if(strstr(tmp, "30"))
	{
		fprintf(stderr, "重定向错误.\n");
	}
	if(strstr(tmp, "400"))
	{
		fprintf(stderr, "请求无效.\n");
	}
	if(strstr(tmp, "401"))
	{
		fprintf(stderr, "未授权.\n");
	}
	if(strstr(tmp, "403"))
	{
		fprintf(stderr, "禁止访问.\n");
	}
	if(strstr(tmp, "404"))
	{
		fprintf(stderr, "无法找到文件.\n");
	}
	if(strstr(tmp, "405"))
	{
		fprintf(stderr, "资源被禁止.\n");
	}
	if(strstr(tmp, "407"))
	{
		fprintf(stderr, "要求代理身份验证.\n");
	}
	if(strstr(tmp, "410"))
	{
		fprintf(stderr, "永远不可用.\n");
	}
	if(strstr(tmp, "414"))
	{
		fprintf(stderr, "请求URI太长.\n");
	}
	if(strstr(tmp, "50"))
	{
		fprintf(stderr, "服务器错误.\n");
	}

	free(tmp);
	return false;
}

long long get_size(char *httphead)
{
	assert(httphead);
	char *delim = "Content-Range: ";

	char *p = strstr(httphead, delim);
	if(p != NULL)
	{
		p += strlen(delim);
		p = strstr(p, "/") + 1;
		return atoll(p);
	}
	return 0LL;
}

long long get_len(char *httphead)
{
	assert(httphead);
	char *delim = "Content-Length: ";

	char *p = strstr(httphead, delim);
	if(p != NULL)
	{
		p += strlen(delim);
		return atoll(p);
	}
	return 0LL;
}

void progress(long long nread, long long filesize)                     //进度条
{
	struct winsize ws;
	ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);

	int bar_len = ws.ws_col-32;
	bar_len = bar_len > 60 ? 60 : bar_len;

	int rate = filesize/bar_len;
	int cur = nread/rate;

	char *total = calloc(1,16);
	if(filesize < 1024)
		snprintf(total, 16, "%llu",filesize);
	else if(filesize >= 1024 && filesize < 1024*1024)
		snprintf(total, 16, "%.lfKB",(float)filesize/1024);
	else if(filesize >= 1024*1024 && filesize < 1024*1024*1024)
		snprintf(total, 16, "%.lfMB",(float)filesize/1024*1024);

	char *bar = calloc(1, 128);
	if(nread < 1024)
		snprintf(bar, 128, "\r[%llu/%s] [", nread, total);
	else if(nread < 1024*1024)
		snprintf(bar, 128, "\r[%.1fKB/%s] [", (float)nread/1024, total);
	else if(nread < 1024*1024*1024)
		snprintf(bar, 128, "\r[%.1fMB/%s] [", (float)nread/1024*1024, total);
	free(total);

	int i;
	for(i=0; i<cur; i++)
		snprintf(bar+strlen(bar), 128-strlen(bar)-i, "%s", "#");
	for(i=0; i<bar_len-cur-1; i++)
		snprintf(bar+strlen(bar), 128-strlen(bar)-i, "%s", "-");
	snprintf(bar+strlen(bar), 128-strlen(bar), "] [%.lf%%]%c", (float)nread/filesize*100,nread==filesize?'\n':' ');
	fprintf(stderr, "%s", bar);
	free(bar);
}
void arg_parser(char *arg, char **host, char **file)
{
	assert(arg);
	assert(host);
	assert(file);

	if(arg[strlen(arg)-1] == '/')
	{
		fprintf(stderr, "illegal\n");
		exit(0);
	}

	char *buf,*sbuf;
	buf = sbuf = arg;

	char *delim1 = "http://";
	char *delim2 = "https://";

	if(strstr(arg, delim1) != NULL)
	{
		buf += strlen(delim1);
	}
	else if(strstr(arg, delim2) != NULL)
	{
		buf += strlen(delim2);
	}
	sbuf = strstr(buf, "/");
	if(sbuf == NULL)
	{
		fprintf(stderr,"illegal\n");
		exit(0);
	}
	sbuf += 1;

	*host = calloc(1,256);
	*file = calloc(1,2048);

	memcpy(*host, buf, sbuf-buf-1);
	memcpy(*file, sbuf, strlen(sbuf));
}

void http_request(char *buf, int size, char *filepath, char *host, int start)
{
	assert(buf);
	bzero(buf, size);
	snprintf(buf, size, "GET /%s "
						"HTTP/1.1\r\n"
						"Range: bytes=%d-\r\n"                             //下载的起始点
						"Host: %s\r\n\r\n",filepath, start, host);
}

int main(int argc, char *argv[])
{
	usage(argc,argv[0]);
	
	//提取主机名，文件路径
	char *host,*file;
	host = file = NULL;
	arg_parser(argv[1],&host,&file);
	
	//通过主机名获取ip地址
	int i;
	struct hostent * p = gethostbyname(host);
	struct in_addr **ipaddr = (struct in_addr **)p->h_addr_list;
	
	//连接服务器
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in cliaddr;
	socklen_t length = sizeof(cliaddr);
	bzero(&cliaddr, length);

	cliaddr.sin_family = AF_INET;
	cliaddr.sin_port = htons(80);
	cliaddr.sin_addr = *ipaddr[0];

	int ret = connect(fd, (struct sockaddr *)&cliaddr, length);

	if(ret == 0)
		printf("连接成功\n");
	else
	{
		printf("连接失败\n");
		exit(0);
	}

	FILE *fp = NULL;
	long long curlen = 0LL;

	char *filename;
	//提取文件名
	if(strstr(file, "/"))
		filename = strrchr(file,'/')+1;             //找到最后一个/
	else
		filename = file;
	//如果文件不存在，创建并以只写方式打开文件
	//否则以追加只写打开已存在的文件
	if(access(filename, F_OK))                      
		fp = fopen(filename,"w");
	else
	{
		struct stat fileinfo;
		stat(filename, &fileinfo);
		curlen = fileinfo.st_size;                   //当前文件大小
		fp = fopen(filename, "a");
	}

	if(NULL == fp)
	{
		perror("fopen failed");
		exit(0);
	}
	setvbuf(fp, NULL, _IONBF, 0);                   //防止程序突然退出，导致缓冲区的数据
													//没来得及写进文件，所以取消缓冲区
													
	//发送请求报文
	char *sndbuf = malloc(1024);
	http_request(sndbuf, 1024, file, host, curlen);

	int n = send(fd, sndbuf, strlen(sndbuf), 0);

	if(n == -1)
	{
		perror("http请求报文发送失败\n");
		exit(0);
	}
	//提取报文头部
	long long total_bytes = curlen;
	char *httphead = calloc(1,1024);
	n = 0;
	while(1)
	{
		read(fd, httphead+n, 1);
		n++;
		if(strstr(httphead, "\r\n\r\n"))               //报文头部是以"\r\n\r\n"结尾
			break;
	}
	
	printf("%s\n",httphead);
	
	//处理http报头
	long long size;
	long long len;
	size = get_size(httphead);                         //整个文件的大小
	len = get_len(httphead);                           //下载内容的大小

	if(!check_response(httphead))
	{
		if(curlen == size&& curlen != 0)
		{
			fprintf(stderr,"文件已下载\n");
		}
		if(!access(filename, F_OK) && curlen == 0)
			remove(filename);
		exit(0);
	}	

	//接收文件
	char *recvbuf = calloc(1, 1024);
	while(1)
	{
		n = recv(fd, recvbuf, 1024, 0);
		if(n == 0)
			break;
		if(n == -1)
		{
			perror("recv() failed");
			exit(0);
		}
		fwrite(recvbuf, n, 1, fp);

		total_bytes += n;
		progress(total_bytes, size);

		if(total_bytes >= size)
			break;

	}
	fclose(fp);
	free(recvbuf);

}
