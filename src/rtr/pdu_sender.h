#ifndef SRC_RTR_PDU_SENDER_H_
#define SRC_RTR_PDU_SENDER_H_

#include <sys/types.h>
#include "pdu.h"

struct sender_common {
	int fd;
	u_int8_t version;
	u_int16_t *session_id;
	u_int32_t *start_serial;
	u_int32_t *end_serial;
};

void init_sender_common(struct sender_common *, int, u_int8_t, u_int16_t *,
    u_int32_t *, u_int32_t *);

int send_serial_notify_pdu(struct sender_common *);
int send_cache_reset_pdu(struct sender_common *);
int send_cache_response_pdu(struct sender_common *);
int send_payload_pdus(struct sender_common *);
int send_end_of_data_pdu(struct sender_common *);
int send_error_report_pdu(int, u_int8_t, u_int16_t, struct pdu_header *,
    char const *);


#endif /* SRC_RTR_PDU_SENDER_H_ */
