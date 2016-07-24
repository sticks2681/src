#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include "zlib.h"
#include "MonitorData.h"

//**********************************************************************************************
//**********************************************************************************************
//**********************************  DO NOT EDIT THIS FILE  ***********************************
//**********************************************************************************************
//**********************************************************************************************

#define BUFFER_SIZE_START		2048
#define BUFFER_RESIZE_VALUE		1024

extern void set_bit(unsigned char p[], int bit);
extern void unset_bit(unsigned char p[], int bit);
extern int  get_bit(unsigned char p[], int bit);

// ------------------   Support Routine  -----------------------------
//////////////////////////////////////////////////////////////////////
//  qsort compare routine
//////////////////////////////////////////////////////////////////////

int compar_name(const void *e1, const void *e2)
{
	MON_ELEMENT *data1, *data2;

	data1 = *(MON_ELEMENT **)e1;
	data2 = *(MON_ELEMENT **)e2;

#ifdef _WIN32
	return _stricmp(data1->label, data2->label);
#else
	return strcasecmp(data1->label, data2->label);
#endif
}

int compar_index(const void *e1, const void *e2)
{
	MON_ELEMENT *data1, *data2;
	data1 = (MON_ELEMENT *)e1;
	data2 = (MON_ELEMENT *)e2;
	return data1->id - data2->id;
}

// ------------------   Game Data Object ----------------------------
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
//CMonitorData::CMonitorData(GenericNotifier *notifier)
//: m_notifier(notifier)
CMonitorData::CMonitorData()
{
	m_sequence = 1;
	m_buffer = nullptr;
	m_nbuffer = 0;
	m_count = 0;
	m_max = ELEMENT_MAX_START;
	m_data = new MON_ELEMENT[m_max];
	for (int x = 0; x < m_max; x++)
	{
		memset(&m_data[x], 0, sizeof(MON_ELEMENT));
	}
	resize_buffer(BUFFER_SIZE_START);
}

CMonitorData::~CMonitorData()
{
	int x;

	for (x = 0; x < m_count; x++)
	{
		delete[] m_data[x].discription;

		delete[] m_data[x].label;
	}

	delete m_buffer;

	delete[] m_data;
}

void CMonitorData::setMax(int _max)
{
	delete[] m_data;

	m_max = _max;
	m_data = new MON_ELEMENT[m_max];
	for (int x = 0; x < m_max; x++)
	{
		memset(&m_data[x], 0, sizeof(MON_ELEMENT));
	}
}

void CMonitorData::resize_buffer(int new_size)
{
	char *temp;

	if (m_buffer != nullptr)
	{
		temp = m_buffer;
		m_buffer = new char[new_size];
		memcpy(m_buffer, temp, m_nbuffer);
		m_nbuffer = new_size;
		delete[] temp;
	}
	else
	{
		m_buffer = new char[new_size];
		m_nbuffer = new_size;
	}
}

void CMonitorData::send(UdpConnection *con, short & sequence, short msg, char *data)
{
	int len;
	unsigned short size;
	char *p;

	size = (unsigned short)strlen(data) + 1;
	p = (char *)malloc(size + 6);
	len = 0;
	packShort(p + len, len, msg);
	packShort(p + len, len, m_sequence);
	packShort(p + len, len, size);
	memcpy(p + len, data, size);
	con->Send(cUdpChannelReliable1, p, size + 6);
	free(p);
	sequence++;
}

void CMonitorData::send(UdpConnection *con, short & sequence, short msg, char *data, int size)
{
	unsigned long compress_len;
	int len;
	unsigned short usize;
	char *p;
	int rnt;

	if (CURRENT_API_VERSION != CURRENT_API_3)
		return;

	p = (char *)malloc(size + 6 + 1000);
	len = 0;
	memset(p, 0, size + 6 + 1000);
	packShort(p + len, len, msg);
	packShort(p + len, len, m_sequence);
	compress_len = size + 1000;
	rnt = compress2((Bytef *)p + 6, &compress_len, (Bytef *)data, (long)size, 2);
	if (rnt != Z_OK)
	{
		free(p);
		printf("CMonitor::send-compress failed, error=%d size =%d\n", rnt, size);
		return;
	}

	usize = (unsigned short)compress_len;
	packShort(p + len, len, usize);
	usize += 6;
	con->Send(cUdpChannelReliable1, p, usize);
	free(p);
	sequence++;
}

bool CMonitorData::processHierarchyRequestBlock(UdpConnection *con, short & sequence)
{
	int x, size, count, len;
	char temp[512];

	if (m_count == 0)
	{
		send(con, sequence, MON_MSG_REPLY_HIERARCHY, "NOTREADY");
		return 0;
	}

	memset(m_buffer, 0, 16);
	send(con, sequence, MON_MSG_REPLY_HIERARCHY_BLOCK_BEGIN, m_buffer);

	MON_ELEMENT	**me;
	me = new MON_ELEMENT *[m_count];
	for (x = 0; x < m_count; x++)
		me[x] = &m_data[x];
	qsort(me, m_count, sizeof(MON_ELEMENT *), compar_name);

	count = x = size = 0;
	while (x < m_count)
	{
		len = sprintf(temp, "%s,%X,%d|", me[x]->label, me[x]->id, me[x]->ping);
		if (len + size >= m_nbuffer) resize_buffer(size + BUFFER_RESIZE_VALUE);
		strcpy(&m_buffer[size], temp);
		size += len;
		if (size > 64000)
		{
			send(con, sequence, MON_MSG_REPLY_HIERARCHY_BLOCK, m_buffer, size + 1);
			memset(m_buffer, 0, size);
			count++;
			size = 0;
		}
		x++;
	}
	delete[] me;
	if (size > 0)
	{
		send(con, sequence, MON_MSG_REPLY_HIERARCHY_BLOCK, m_buffer, size + 1);
		memset(m_buffer, 0, 16);
		count++;
		size = 0;
	}
	memset(m_buffer, 0, 16);
	send(con, sequence, MON_MSG_REPLY_HIERARCHY_BLOCK_END, m_buffer);
	return 1;
}

bool CMonitorData::processElementsRequest(
	UdpConnection *con, short & sequence, char * data, int /* dataLen */, long lastUpdateTime)
{
	char	tmp[200];
	int		x, id;
	int     size;
	int		count;
	char	**list;

	memset(m_buffer, 0, 16);
	if (!strcmp(data, "-1"))
	{
		size = 0;
		for (x = 0; x < m_count; x++)
		{
			sprintf(tmp, "%d %x|", m_data[x].id, m_data[x].value);
			size += (int)strlen(tmp);
			if (size >= m_nbuffer)
				resize_buffer(size + BUFFER_RESIZE_VALUE);
			strcat(m_buffer, tmp);
			if (size > 65000)
			{
				send(con, sequence, MON_MSG_REPLY_ELEMENTS, m_buffer);
				memset(m_buffer, 0, 16);
			}
		}
		if (size)
			send(con, sequence, MON_MSG_REPLY_ELEMENTS, m_buffer);

		return 1;
	}

	if (!strcmp(data, "-2"))
	{
		size = 0;
		count = 0;
		for (x = 0; x < m_count; x++)
		{
			if (lastUpdateTime <= m_data[x].ChangedTime)
			{
				sprintf(tmp, "%d %x|", m_data[x].id, m_data[x].value);
				size += (int)strlen(tmp);
				if (size >= m_nbuffer)
					resize_buffer(size + BUFFER_RESIZE_VALUE);
				strcat(m_buffer, tmp);
				count++;
			}

			if (size > 65000)
			{
				send(con, sequence, MON_MSG_REPLY_ELEMENTS, m_buffer);
				memset(m_buffer, 0, 16);
			}
		}
		if (count)
		{
			send(con, sequence, MON_MSG_REPLY_ELEMENTS, m_buffer);
			return 1;
		}
		return 0;
	}

	list = new char *[m_max];
	count = parseList(list, data, '|', m_max);
	memset(m_buffer, 0, 16);

	size = 0;
	for (x = 0; x < count; x++)
	{
		id = atoi(list[x]);

		for (int y = 0; y < m_count; y++)
			if (m_data[y].id == id)
			{
				sprintf(tmp, "%d %x|", m_data[y].id, m_data[y].value);
				size += (int)strlen(tmp);
				if (size >= m_nbuffer)
					resize_buffer(size + BUFFER_RESIZE_VALUE);
				strcat(m_buffer, tmp);
			}
	}

	delete[] list;

	if (count > 0)
		send(con, sequence, MON_MSG_REPLY_ELEMENTS, m_buffer);

	return 0;
}

bool CMonitorData::processDescriptionRequest(UdpConnection *con, short & sequence, char * userData, int, unsigned char *mark)
{
	char line[4096];
	char tmp[400];
	int	 x, id, size;
	int flag;
	char *p;

	size = 0;
	memset(m_buffer, 0, 16);
	strcpy(line, userData);
	p = strtok(line, "|");
	flag = 0;
	if (strstr(p, "next"))
	{
		for (x = 0; x < m_count; x++)
		{
			if (get_bit(mark, x) == 0)
			{
				set_bit(mark, x);
				if (m_data[x].discription != nullptr)
				{
					sprintf(tmp, "%d,%s|", m_data[x].id, m_data[x].discription);
					size += (int)strlen(tmp);
					if (size >= m_nbuffer)
						resize_buffer(size + BUFFER_RESIZE_VALUE);
					strcat(m_buffer, tmp);
					send(con, sequence, MON_MSG_REPLY_DESCRIPTION, m_buffer);
					return 1;
				}
			}
		}
	}
	else
	{
		if (p)
		{
			size = 0;
			flag = 0;
			id = atoi(p);
			for (x = 0; x < m_count; x++)
			{
				if (flag == 0 && id == m_data[x].id)
					flag = 1;

				if (flag)
				{
					flag = 2;
					set_bit(mark, x);
					if (m_data[x].discription != nullptr)
					{
						sprintf(tmp, "%d,%s|", m_data[x].id, m_data[x].discription);
						size += (int)strlen(tmp);
						if (size >= m_nbuffer)
							resize_buffer(size + BUFFER_RESIZE_VALUE);
						strcat(m_buffer, tmp);
						if (size >= 2000 || m_data[x].discription)
						{
							send(con, sequence, MON_MSG_REPLY_DESCRIPTION, m_buffer);
							return 1;
						}
					}
				}
			}
		}
	}

	if (flag == 2)
		send(con, sequence, MON_MSG_REPLY_DESCRIPTION, m_buffer);
	send(con, sequence, MON_MSG_REPLY_DESCRIPTION, "none");
	return 0;
}

int CMonitorData::add(const char *label, int id, int ping, const char *des)
{
	int x;

	if (strlen(label) >= 127)
	{
		printf("MonAPI Error: add() label exceeds length of 127. ->%s\n", label);
		return 0;
	}

	for (x = 0; x < m_count; x++)
	{
		if (id == m_data[x].id)
		{
			printf("MonAPI: assign new Id (%d) %s -> %s\n", id, m_data[x].label, label);
			m_data[x].ping = pingValue(ping);
			m_data[x].value = 0;
			m_data[x].id = id;

			//*******  Label  ***********
			delete[] m_data[x].label;
			m_data[x].label = new char[strlen(label) + 1];
			strcpy(m_data[x].label, label);

			//*******  Discription ******
			delete[] m_data[x].discription;
			m_data[x].discription = nullptr;
			if (des)
			{
				m_data[x].discription = new char[strlen(des) + 1];
				strcpy(m_data[x].discription, des);
			}
			qsort(m_data, m_count, sizeof(MON_ELEMENT), compar_index);
			return 1;
		}

		if (!strcmp(label, m_data[x].label))
		{
			printf("MonAPI ERROR: Label already found %s  Id: old(%d) new(%d).\n", label, m_data[x].id, id);
			return 0;
		}
	}

	if (m_max == m_count + 1)
	{
		printf("MonitorObject: max element reached [%d].\n%s\nContact David Taylor (858) 577-3155.\n", m_max, label);
		return 0;
	}
	m_data[m_count].ping = pingValue(ping);
	m_data[m_count].value = 0;
	m_data[m_count].id = id;
	delete[] m_data[m_count].label;
	m_data[m_count].label = new char[strlen(label) + 1];
	strcpy(m_data[m_count].label, label);
	if (des)
	{
		delete[] m_data[x].discription;
		m_data[x].discription = new char[strlen(des) + 1];
		strcpy(m_data[x].discription, des);
	}
	m_count++;
	qsort(m_data, m_count, sizeof(MON_ELEMENT), compar_index);
	return 1;
}

int CMonitorData::setDescription(int Id, const char *Description, int & mode)
{
	int x;

	for (x = 0; x < m_count; x++)
	{
		if (Id == m_data[x].id)
		{
			if (Description == nullptr)
			{
				delete[] m_data[x].discription;
				m_data[x].discription = nullptr;
				mode = 0;
				return x;
			}
			if (m_data[x].discription && !strcmp(m_data[x].discription, Description))
				return -1;

			delete[] m_data[x].discription;
			m_data[x].discription = new char[strlen(Description) + 1];
			strcpy(m_data[x].discription, Description);
			mode = 1;
			return x;
		}
	}
	return -1;
}

int CMonitorData::pingValue(int p)
{
	switch (p)
	{
	case MON_HISTORY: return (60 * 5);
	case MON_ONLY_SHOW:	return 0;
	default:
		printf("MonAPI ERROR: add() function is not using defined constances.\n");
		printf("Please use one of the following:\n");
		printf("\tMON_PING_15\n\tMON_PING_30\n\tMON_PING_60\n\tMON_PING_5MIN\n");
		break;
	}
	return 0;
}

void CMonitorData::set(int Id, int value)
{
	int high, i, low;

	for (low = (-1), high = m_count; high - low > 1; )
	{
		i = (high + low) / 2;
		if (Id <= m_data[i].id)
			high = i;
		else
			low = i;
	}

	if (high < m_count &&  Id == m_data[high].id)
	{
		if (m_data[high].ChangedTime == 0 || m_data[high].value != value) {
			m_data[high].ChangedTime = (long)time(nullptr);
			m_data[high].value = value;
		}
	}
}

void CMonitorData::remove(int Id)
{
	int x;

	if (m_count == 1)
	{
		delete[] m_data[0].label;

		delete[] m_data[0].discription;

		m_data[0].label = 0;
		m_data[0].id = 0;
		m_data[0].value = 0;;
		m_data[0].ping = 0;;
		m_count--;
		return;
	}

	for (x = 0; x < m_count; x++)
	{
		if (Id == m_data[x].id)
		{
			delete[] m_data[x].label;

			delete[] m_data[x].discription;

			m_data[x].label = 0;
			if (x < m_count - 1)
			{
				memcpy(&m_data[x], &m_data[m_count - 1], sizeof(MON_ELEMENT));
			}
			memset(&m_data[m_count - 1], 0, sizeof(MON_ELEMENT));
			m_count--;
			return;
		}
	}
}

int CMonitorData::parseList(char **list, char *data, char tok, int max)
{
	int count;
	int cnt;

	if (data == nullptr)
		return 0;

	list[0] = data;
	count = 1;
	cnt = 0;
	while (cnt < max && *data > 0)
	{
		if (*data == tok)
		{
			*data = 0;
			data++;
			cnt++;
			list[count] = data;
			if (*data)	count++;
		}
		cnt++;
		if (*data)	data++;
	}
	return count;
}

void CMonitorData::dump()
{
	printf("********** Monitor API Dump *******************\n");
	printf("  Version: %d\n", CURRENT_API_VERSION);
	printf("\ncount: %d\n", m_count);
	printf("\t%-40s %8s\t%s\t%s\n", "Label", "Id", "Ping", "Value");
	for (int x = 0; x < m_count; x++)
	{
		printf("\t%-40s % 8d\t%d\t%d\n",
			m_data[x].label,
			m_data[x].id,
			m_data[x].value,
			m_data[x].ping);
	}
	printf("***********************************************\n");
}

/*
int CMonitorData::compress(char *dest,unsigned long *destLen,const char *source,long sourceLen,int level)
{
z_stream stream;
int err;

	stream.next_in = (Bytef*)source;
	stream.avail_in = (uInt)sourceLen;
	stream.next_out = (Bytef *)dest;
	stream.avail_out = (uInt)*destLen;
	if ((uLong)stream.avail_out != *destLen) return Z_BUF_ERROR;

	stream.zalloc = (alloc_func)0;
	stream.zfree = (free_func)0;
	stream.opaque = (voidpf)0;

	err = deflateInit(&stream, level);
	if (err != Z_OK) return err;

	err = deflate(&stream, Z_FINISH);
	if (err != Z_STREAM_END) {
		deflateEnd(&stream);
		return err == Z_OK ? Z_BUF_ERROR : err;
	}
	*destLen = stream.total_out;

	err = deflateEnd(&stream);
	return err;
}
*/

//////////////////////////////////////////////////////////////////////////////
// ------------------   Pack and unpack Routine  -----------------------------
///////////////////////////////////////////////////////////////////////////////

int packString(char *buffer, int & len, char * value)
{
	int size;
	size = (int)strlen(value) + 1;
	memcpy(buffer, value, size);
	len += size;
	return size;
}

int packByte(char *buffer, int & len, char value)
{
	buffer[0] = value;
	len++;
	return 1;
}

int packShort(char *buffer, int & len, short value)
{
	char *p;

	p = (char *)&value;

#ifdef  _sun
	buffer[0] = p[1];
	buffer[1] = p[0];
#else
	buffer[0] = p[0];
	buffer[1] = p[1];
#endif

	len += 2;
	return 2;
}

int packShort(char *buffer, int & len, unsigned short value)
{
	char *p;

	p = (char *)&value;

#ifdef  _sun
	buffer[0] = p[1];
	buffer[1] = p[0];
#else
	buffer[0] = p[0];
	buffer[1] = p[1];
#endif

	len += 2;
	return 2;
}

int unpackShort(char *buffer, int & len, short & value)
{
	char *p;
	p = (char *)&value;

#ifdef _sun
	p[1] = buffer[0];
	p[0] = buffer[1];

#else
	p[0] = buffer[0];
	p[1] = buffer[1];
#endif

	len += 2;
	return 2;
}

int unpackShort(char *buffer, int & len, unsigned short & value)
{
	char *p;
	p = (char *)&value;

#ifdef _sun
	p[1] = buffer[0];
	p[0] = buffer[1];

#else
	p[0] = buffer[0];
	p[1] = buffer[1];
#endif

	len += 2;
	return 2;
}

int unpackByte(char *buffer, int & len, char &value)
{
	value = buffer[0];
	len++;
	return 1;
}
