#define DNS_MODE_STA 0x01
#define DNS_MODE_AP  0x02
#define DNS_MODE_STATIONAP  0x03

void dns_resp_init(uint8_t mode);

int get_A_Record(uint8_t addr[4], const char domain_name[]);
int get_AAAA_Record(uint8_t addr[16], const char domain_name[]);
