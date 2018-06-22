#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_interface.h"

#include "espconn.h"
//#include "user_json.h"
//#include "user_devicefind.h"

#define BUF_SIZE 1500

/*
* This software is licensed under the CC0.
*
* This is a _basic_ DNS Server for educational use.
* It does not prevent invalid packets from crashing
* the server.
*
* To test start the program and issue a DNS request:
*  dig @127.0.0.1 -p 9000 foo.bar.com 
*/


/*
* Masks and constants.
*/

static const uint32_t QR_MASK = 0x8000;
static const uint32_t OPCODE_MASK = 0x7800;
static const uint32_t AA_MASK = 0x0400;
static const uint32_t TC_MASK = 0x0200;
static const uint32_t RD_MASK = 0x0100;
static const uint32_t RA_MASK = 0x8000;
static const uint32_t RCODE_MASK = 0x000F;

/* Response Type */
enum {
  Ok_ResponseType = 0,
  FormatError_ResponseType = 1,
  ServerFailure_ResponseType = 2,
  NameError_ResponseType = 3,
  NotImplemented_ResponseType = 4,
  Refused_ResponseType = 5
};

/* Resource Record Types */
enum {
  A_Resource_RecordType = 1,
  NS_Resource_RecordType = 2,
  CNAME_Resource_RecordType = 5,
  SOA_Resource_RecordType = 6,
  PTR_Resource_RecordType = 12,
  MX_Resource_RecordType = 15,
  TXT_Resource_RecordType = 16,
  AAAA_Resource_RecordType = 28,
  SRV_Resource_RecordType = 33
};

/* Operation Code */
enum {
  QUERY_OperationCode = 0, /* standard query */
  IQUERY_OperationCode = 1, /* inverse query */
  STATUS_OperationCode = 2, /* server status request */
  NOTIFY_OperationCode = 4, /* request zone transfer */
  UPDATE_OperationCode = 5 /* change resource records */
};

/* Response Code */
enum {
  NoError_ResponseCode = 0,
  FormatError_ResponseCode = 1,
  ServerFailure_ResponseCode = 2,
  NameError_ResponseCode = 3
};

/* Query Type */
enum {
  IXFR_QueryType = 251,
  AXFR_QueryType = 252,
  MAILB_QueryType = 253,
  MAILA_QueryType = 254,
  STAR_QueryType = 255
};

/*
* Types.
*/

/* Question Section */
struct Question {
  char *qName;
  uint16_t qType;
  uint16_t qClass;
  struct Question* next; // for linked list
};

/* Data part of a Resource Record */
union ResourceData {
  struct {
    char *txt_data;
  } txt_record;
  struct {
    uint8_t addr[4];
  } a_record;
  struct {
    char* MName;
    char* RName;
    uint32_t serial;
    uint32_t refresh;
    uint32_t retry;
    uint32_t expire;
    uint32_t minimum;
  } soa_record;
  struct {
    char *name;
  } name_server_record;
  struct {
    char name;
  } cname_record;
  struct {
    char *name;
  } ptr_record;
  struct {
    uint16_t preference;
    char *exchange;
  } mx_record;
  struct {
    uint8_t addr[16];
  } aaaa_record;
  struct {
    uint16_t priority;
    uint16_t weight;
    uint16_t port;
    char *target;
  } srv_record;
};

/* Resource Record Section */
struct ResourceRecord {
  char *name;
  uint16_t type;
  uint16_t class;
  uint16_t ttl;
  uint16_t rd_length;
  union ResourceData rd_data;
  struct ResourceRecord* next; // for linked list
};

struct Message {
  uint16_t id; /* Identifier */

  /* Flags */
  uint16_t qr; /* Query/Response Flag */
  uint16_t opcode; /* Operation Code */
  uint16_t aa; /* Authoritative Answer Flag */
  uint16_t tc; /* Truncation Flag */
  uint16_t rd; /* Recursion Desired */
  uint16_t ra; /* Recursion Available */
  uint16_t rcode; /* Response Code */

  uint16_t qdCount; /* Question Count */
  uint16_t anCount; /* Answer Record Count */
  uint16_t nsCount; /* Authority Record Count */
  uint16_t arCount; /* Additional Record Count */

  /* At least one question; questions are copied to the response 1:1 */
  struct Question* questions;

  /*
  * Resource records to be send back.
  * Every resource record can be in any of the following places.
  * But every place has a different semantic.
  */
  struct ResourceRecord* answers;
  struct ResourceRecord* authorities;
  struct ResourceRecord* additionals;
};

int ICACHE_FLASH_ATTR get_A_Record(uint8_t addr[4], const char domain_name[])
{
  if (strcmp("foo.bar.com", domain_name) == 0)
  {
    addr[0] = 192;
    addr[1] = 168;
    addr[2] = 1;
    addr[3] = 1;
    return 0;
  }
  else
  {
    return -1;
  }
}

int ICACHE_FLASH_ATTR get_AAAA_Record(uint8_t addr[16], const char domain_name[])
{
  if (strcmp("foo.bar.com", domain_name) == 0)
  {
    addr[0] = 0xfe;
    addr[1] = 0x80;
    addr[2] = 0x00;
    addr[3] = 0x00;
    addr[4] = 0x00;
    addr[5] = 0x00;
    addr[6] = 0x00;
    addr[7] = 0x00;
    addr[8] = 0x00;
    addr[9] = 0x00;
    addr[10] = 0x00;
    addr[11] = 0x00;
    addr[12] = 0x00;
    addr[13] = 0x00;
    addr[14] = 0x00;
    addr[15] = 0x01;
    return 0;
  }
  else
  {
    return -1;
  }
}


/*
* Debugging functions.
*/

void ICACHE_FLASH_ATTR print_hex(uint8_t* buf, size_t len)
{
  int i;
  os_printf("%zu bytes:\n", len);
  for(i = 0; i < len; ++i)
    os_printf("%02x ", buf[i]);
  os_printf("\n");
}

void print_resource_record(struct ResourceRecord* rr)
{
  int i;
  while (rr)
  {
    os_printf("  ResourceRecord { name '%s', type %u, class %u, ttl %u, rd_length %u, ",
        rr->name,
        rr->type,
        rr->class,
        rr->ttl,
        rr->rd_length
   );

    union ResourceData *rd = &rr->rd_data;
    switch (rr->type)
    {
      case A_Resource_RecordType:
        os_printf("Address Resource Record { address ");

        for(i = 0; i < 4; ++i)
          os_printf("%s%u", (i ? "." : ""), rd->a_record.addr[i]);

        os_printf(" }");
        break;
      case NS_Resource_RecordType:
        os_printf("Name Server Resource Record { name %s }",
          rd->name_server_record.name
       );
        break;
      case CNAME_Resource_RecordType:
        os_printf("Canonical Name Resource Record { name %u }",
          rd->cname_record.name
       );
        break;
      case SOA_Resource_RecordType:
        os_printf("SOA { MName '%s', RName '%s', serial %u, refresh %u, retry %u, expire %u, minimum %u }",
          rd->soa_record.MName,
          rd->soa_record.RName,
          rd->soa_record.serial,
          rd->soa_record.refresh,
          rd->soa_record.retry,
          rd->soa_record.expire,
          rd->soa_record.minimum
       );
        break;
      case PTR_Resource_RecordType:
        os_printf("Pointer Resource Record { name '%s' }",
          rd->ptr_record.name
       );
        break;
      case MX_Resource_RecordType:
        os_printf("Mail Exchange Record { preference %u, exchange '%s' }",
          rd->mx_record.preference,
          rd->mx_record.exchange
       );
        break;
      case TXT_Resource_RecordType:
        os_printf("Text Resource Record { txt_data '%s' }",
          rd->txt_record.txt_data
       );
        break;
      case AAAA_Resource_RecordType:
        os_printf("AAAA Resource Record { address ");

        for(i = 0; i < 16; ++i)
          os_printf("%s%02x", (i ? ":" : ""), rd->aaaa_record.addr[i]);

        os_printf(" }");
        break;
      default:
        os_printf("Unknown Resource Record { ??? }");
    }
    os_printf("}\n");
    rr = rr->next;
  }
}

void ICACHE_FLASH_ATTR print_query(struct Message* msg)
{
  os_printf("QUERY { ID: %02x", msg->id);
  os_printf(". FIELDS: [ QR: %u, OpCode: %u ]", msg->qr, msg->opcode);
  os_printf(", QDcount: %u", msg->qdCount);
  os_printf(", ANcount: %u", msg->anCount);
  os_printf(", NScount: %u", msg->nsCount);
  os_printf(", ARcount: %u,\n", msg->arCount);

  struct Question* q = msg->questions;
  while (q)
  {
    os_printf("  Question { qName '%s', qType %u, qClass %u }\n",
      q->qName,
      q->qType,
      q->qClass
   );
    q = q->next;
  }

  print_resource_record(msg->answers);
  print_resource_record(msg->authorities);
  print_resource_record(msg->additionals);

  os_printf("}\n");
}


/*
* Basic memory operations.
*/

size_t ICACHE_FLASH_ATTR get16bits(const uint8_t** buffer)
{
  uint16_t value;

  os_memcpy(&value, *buffer, 2);
  *buffer += 2;

  return ntohs(value);
}

void ICACHE_FLASH_ATTR put8bits(uint8_t** buffer, uint8_t value)
{
  os_memcpy(*buffer, &value, 1);
  *buffer += 1;
}

void ICACHE_FLASH_ATTR put16bits(uint8_t** buffer, uint16_t value)
{
  value = htons(value);
  os_memcpy(*buffer, &value, 2);
  *buffer += 2;
}

void ICACHE_FLASH_ATTR put32bits(uint8_t** buffer, uint32_t value)
{
  value = htons(value);
  os_memcpy(*buffer, &value, 4);
  *buffer += 4;
}


/*
* Deconding/Encoding functions.
*/

// 3foo3bar3com0 => foo.bar.com
char* ICACHE_FLASH_ATTR decode_domain_name(const uint8_t** buffer)
{
  char name[256];
  const uint8_t* buf = *buffer;
  int j = 0;
  int i = 0;

  while (buf[i] != 0)
  {
    //if (i >= buflen || i > sizeof(name))
    //  return NULL;

    if (i != 0)
    {
      name[j] = '.';
      j += 1;
    }

    int len = buf[i];
    i += 1;

    os_memcpy(name+j, buf+i, len);
    i += len;
    j += len;
  }

  name[j] = '\0';

  *buffer += i + 1; //also jump over the last 0

  //return strdup(name);
  uint8_t *ret = os_malloc(os_strlen(name)+1);
  os_strcpy(ret, name);
  return ret;
}

char ICACHE_FLASH_ATTR *_strchr(const char *s, int c) {
    while (*s != (char)c)
	if (!*s++)
	    return 0;
    return (char *)s;
}

// foo.bar.com => 3foo3bar3com0
void ICACHE_FLASH_ATTR encode_domain_name(uint8_t** buffer, const char* domain)
{
  uint8_t* buf = *buffer;
  const char* beg = domain;
  const char* pos;
  int len = 0;
  int i = 0;

  while ((pos = _strchr(beg, '.')))
  {
    len = pos - beg;
    buf[i] = len;
    i += 1;
    os_memcpy(buf+i, beg, len);
    i += len;

    beg = pos + 1;
  }

  len = os_strlen(domain) - (beg - domain);

  buf[i] = len;
  i += 1;

  os_memcpy(buf + i, beg, len);
  i += len;

  buf[i] = 0;
  i += 1;

  *buffer += i;
}


void ICACHE_FLASH_ATTR decode_header(struct Message* msg, const uint8_t** buffer)
{
  msg->id = get16bits(buffer);

  uint32_t fields = get16bits(buffer);
  msg->qr = (fields & QR_MASK) >> 15;
  msg->opcode = (fields & OPCODE_MASK) >> 11;
  msg->aa = (fields & AA_MASK) >> 10;
  msg->tc = (fields & TC_MASK) >> 9;
  msg->rd = (fields & RD_MASK) >> 8;
  msg->ra = (fields & RA_MASK) >> 7;
  msg->rcode = (fields & RCODE_MASK) >> 0;

  msg->qdCount = get16bits(buffer);
  msg->anCount = get16bits(buffer);
  msg->nsCount = get16bits(buffer);
  msg->arCount = get16bits(buffer);
}

void ICACHE_FLASH_ATTR encode_header(struct Message* msg, uint8_t** buffer)
{
  put16bits(buffer, msg->id);

  int fields = 0;
  fields |= (msg->qr << 15) & QR_MASK;
  fields |= (msg->rcode << 0) & RCODE_MASK;
  // TODO: insert the rest of the fields
  put16bits(buffer, fields);

  put16bits(buffer, msg->qdCount);
  put16bits(buffer, msg->anCount);
  put16bits(buffer, msg->nsCount);
  put16bits(buffer, msg->arCount);
}

int ICACHE_FLASH_ATTR decode_msg(struct Message* msg, const uint8_t* buffer, int size)
{
  int i;

  decode_header(msg, &buffer);

  if (msg->anCount != 0 || msg->nsCount != 0)
  {
    os_printf("Only questions expected!\n");
    return -1;
  }

  // parse questions
  uint32_t qcount = msg->qdCount;
  struct Question* qs = msg->questions;
  for (i = 0; i < qcount; ++i)
  {
    struct Question* q = os_malloc(sizeof(struct Question));

    q->qName = decode_domain_name(&buffer);
    q->qType = get16bits(&buffer);
    q->qClass = get16bits(&buffer);

    // prepend question to questions list
    q->next = qs;
    msg->questions = q;
  }

  // We do not expect any resource records to parse here.

  return 0;
}

// For every question in the message add a appropiate resource record
// in either section 'answers', 'authorities' or 'additionals'.
void ICACHE_FLASH_ATTR resolver_process(struct Message* msg)
{
  struct ResourceRecord* beg;
  struct ResourceRecord* rr;
  struct Question* q;
  int rc;

  // leave most values intact for response
  msg->qr = 1; // this is a response
  msg->aa = 1; // this server is authoritative
  msg->ra = 0; // no recursion available
  msg->rcode = Ok_ResponseType;

  // should already be 0
  msg->anCount = 0;
  msg->nsCount = 0;
  msg->arCount = 0;

  // for every question append resource records
  q = msg->questions;
  while (q)
  {
    rr = os_malloc(sizeof(struct ResourceRecord));
    os_memset(rr, 0, sizeof(struct ResourceRecord));

    //rr->name = strdup(q->qName);
    rr->name = os_malloc(os_strlen(q->qName)+1);
    os_strcpy(rr->name, q->qName);
    rr->type = q->qType;
    rr->class = q->qClass;
    rr->ttl = 60*60; // in seconds; 0 means no caching

    os_printf("Query for '%s'\n", q->qName);

    // We only can only answer two question types so far
    // and the answer (resource records) will be all put
    // into the answers list.
    // This behavior is probably non-standard!
    switch (q->qType)
    {
      case A_Resource_RecordType:
        rr->rd_length = 4;
        rc = get_A_Record(rr->rd_data.a_record.addr, q->qName);
        if (rc < 0)
        {
          os_free(rr->name);
          os_free(rr);
          goto next;
        }
        break;
      case AAAA_Resource_RecordType:
        rr->rd_length = 16;
        rc = get_AAAA_Record(rr->rd_data.aaaa_record.addr, q->qName);
        if (rc < 0)
        {
          os_free(rr->name);
          os_free(rr);
          goto next;
        }
        break;
      /*
      case NS_Resource_RecordType:
      case CNAME_Resource_RecordType:
      case SOA_Resource_RecordType:
      case PTR_Resource_RecordType:
      case MX_Resource_RecordType:
      case TXT_Resource_RecordType:
      */
      default:
        os_free(rr);
        msg->rcode = NotImplemented_ResponseType;
        os_printf("Cannot answer question of type %d.\n", q->qType);
        goto next;
    }

    msg->anCount++;

    // prepend resource record to answers list
    beg = msg->answers;
    msg->answers = rr;
    rr->next = beg;

    // jump here to omit question
    next:

    // process next question
    q = q->next;
  }
}

/* @return 0 upon failure, 1 upon success */
int ICACHE_FLASH_ATTR encode_resource_records(struct ResourceRecord* rr, uint8_t** buffer)
{
  int i;
  while (rr)
  {
    // Answer questions by attaching resource sections.
    encode_domain_name(buffer, rr->name);
    put16bits(buffer, rr->type);
    put16bits(buffer, rr->class);
    put32bits(buffer, rr->ttl);
    put16bits(buffer, rr->rd_length);

    switch (rr->type)
    {
      case A_Resource_RecordType:
        for(i = 0; i < 4; ++i)
          put8bits(buffer, rr->rd_data.a_record.addr[i]);
        break;
      case AAAA_Resource_RecordType:
        for(i = 0; i < 16; ++i)
          put8bits(buffer, rr->rd_data.aaaa_record.addr[i]);
        break;
      default:
        os_printf("Unknown type %u. => Ignore resource record.\n", rr->type);
      return 1;
    }

    rr = rr->next;
  }

  return 0;
}

/* @return 0 upon failure, 1 upon success */
int ICACHE_FLASH_ATTR encode_msg(struct Message* msg, uint8_t** buffer)
{
  struct Question* q;
  int rc;

  encode_header(msg, buffer);

  q = msg->questions;
  while (q)
  {
    encode_domain_name(buffer, q->qName);
    put16bits(buffer, q->qType);
    put16bits(buffer, q->qClass);

    q = q->next;
  }

  rc = 0;
  rc |= encode_resource_records(msg->answers, buffer);
  rc |= encode_resource_records(msg->authorities, buffer);
  rc |= encode_resource_records(msg->additionals, buffer);

  return rc;
}

void ICACHE_FLASH_ATTR free_resource_records(struct ResourceRecord* rr)
{
  struct ResourceRecord* next;

  while (rr) {
    os_free(rr->name);
    next = rr->next;
    os_free(rr);
    rr = next;
  }
}

void ICACHE_FLASH_ATTR free_questions(struct Question* qq)
{
  struct Question* next;

  while (qq) {
    os_free(qq->qName);
    next = qq->next;
    os_free(qq);
    qq = next;
  }
}

/*---------------------------------------------------------------------------*/
LOCAL struct espconn dns_espconn;
struct Message msg;

LOCAL void ICACHE_FLASH_ATTR
user_udp_recv(void *arg, char *pusrdata, unsigned short length)
{
    uint8_t hwaddr[6];
    uint8_t buffer[1024];
 
    struct ip_info ipconfig;
    struct espconn *pespconn = (struct espconn *)arg;
 
    if (wifi_get_opmode() != STATION_MODE) 
   {
        wifi_get_ip_info(SOFTAP_IF, &ipconfig);
        wifi_get_macaddr(SOFTAP_IF, hwaddr);
 
        if (!ip_addr_netcmp((struct ip_addr *)pespconn->proto.udp->remote_ip, &ipconfig.ip, &ipconfig.netmask)) 
      {
         //udp packet is received from ESP8266 station
            wifi_get_ip_info(STATION_IF, &ipconfig);
            wifi_get_macaddr(STATION_IF, hwaddr);
        }
      else
      {
         //udp packet is received from ESP8266 softAP
        }
       
    } 
   else
   {
      //udp packet is received from ESP8266 station
        wifi_get_ip_info(STATION_IF, &ipconfig);
        wifi_get_macaddr(STATION_IF, hwaddr);
    }
 
    if (pusrdata == NULL) 
        return;

    free_questions(msg.questions);
    free_resource_records(msg.answers);
    free_resource_records(msg.authorities);
    free_resource_records(msg.additionals);
    os_memset(&msg, 0, sizeof(struct Message));

    if (decode_msg(&msg, pusrdata, length) != 0) {
        return;
    }

    /* Print query */
    print_query(&msg);

    resolver_process(&msg);

    /* Print response */
    print_query(&msg);

    uint8_t *p = buffer;
    if (encode_msg(&msg, &p) != 0) {
        return;
    }

    espconn_sent(pespconn, buffer, p - buffer);
}
 
void ICACHE_FLASH_ATTR
dns_resp_init(void)
{
    dns_espconn.type = ESPCONN_UDP;
    dns_espconn.proto.udp = (esp_udp *)os_zalloc(sizeof(esp_udp));
    dns_espconn.proto.udp->local_port = 52;  // DNS udp port
    espconn_regist_recvcb(&dns_espconn, user_udp_recv); // register a udp packet receiving callback
    espconn_create(&dns_espconn);   // create udp
}

