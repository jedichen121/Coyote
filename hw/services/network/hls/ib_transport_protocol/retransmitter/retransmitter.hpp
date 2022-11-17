/************************************************
Copyright (c) 2019, Systems Group, ETH Zurich.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************/
#pragma once

#include "../../axi_utils.hpp"
#include "../ib_transport_protocol.hpp"
using namespace hls;

const uint32_t META_TABLE_SIZE = 2000;

struct retransEvent;

struct retransRelease
{
	ap_uint<16> qpn;
	ap_uint<24> latest_acked_req; //TODO rename?
	retransRelease() {};
	retransRelease(ap_uint<16> qpn, ap_uint<24> psn)
		:qpn(qpn), latest_acked_req(psn) {}
};

struct retransmission
{
	ap_uint<16> qpn;
	ap_uint<24> psn;
	//bool		implicit; //TODO or remove
	retransmission() {}
	retransmission(ap_uint<16> qpn)
		:qpn(qpn), psn(0) {}
	retransmission(ap_uint<16> qpn, ap_uint<24> psn)
		:qpn(qpn), psn(psn) {}
};

struct retransMeta
{
	ap_uint<16> qpn;
	ap_uint<24> psn;
	ibOpCode	opCode;
	retransMeta() {}
	retransMeta(ap_uint<16> qpn, ap_uint<24> psn, ibOpCode op)
		:qpn(qpn), psn(psn), opCode(op){}
};

struct retransAddrLen
{
	ap_uint<48> localAddr;
	ap_uint<48> remoteAddr;
	ap_uint<32> length;
	retransAddrLen() {}
	retransAddrLen(ap_uint<48> laddr, ap_uint<48> raddr, ap_uint<32> len)
		:localAddr(laddr), remoteAddr(raddr), length(len) {}
};

struct retransEntry
{
	ap_uint<16> qpn;
	ap_uint<24> psn;
	ibOpCode	opCode;
	ap_uint<48> localAddr;
	ap_uint<48> remoteAddr;
	ap_uint<32> length;
	retransEntry() {}
	retransEntry(ap_uint<16> qpn, ap_uint<24> psn, ibOpCode op, ap_uint<64> laddr, ap_uint<64> raddr, ap_uint<32> len)
		:qpn(qpn), psn(psn), opCode(op), localAddr(laddr), remoteAddr(raddr), length(len) {}
	retransEntry(retransMeta meta, retransAddrLen addrlen)
		:qpn(meta.qpn), psn(meta.psn), opCode(meta.opCode), localAddr(addrlen.localAddr), remoteAddr(addrlen.remoteAddr), length(addrlen.length) {}
};

struct retransPointerEntry
{
	ap_uint<16>	head;
	ap_uint<16> tail;
	bool valid;
};

typedef enum {INSERT, RELEASE, RX_RETRANS, TIMER_RETRANS} retransOperation ;

struct pointerMeta
{
	retransOperation	op;
	//ap_uint<16>			qpn;
	ap_uint<24>			psn;
	retransPointerEntry entry;
	pointerMeta() {}
	pointerMeta(retransOperation op, ap_uint<24> psn, retransPointerEntry e)
		:op(op), psn(psn), entry(e) {}
};

struct pointerUpdate
{
	ap_uint<16>	qpn;
	retransPointerEntry entry;
	pointerUpdate() {}
	pointerUpdate(ap_uint<16> qpn, retransPointerEntry e)
		:qpn(qpn), entry(e) {}
};

struct retransMetaEntry
{
	ap_uint<24> psn;
	ap_uint<16> next;
	ibOpCode	opCode;
	ap_uint<48> localAddr;
	ap_uint<48> remoteAddr;
	ap_uint<32> length;
	bool valid;
	bool isTail;
	retransMetaEntry() {}
	retransMetaEntry(retransEntry& e)
		:psn(e.psn), next(0), opCode(e.opCode), localAddr(e.localAddr), remoteAddr(e.remoteAddr), length(e.length), valid(true), isTail(true) {}
	retransMetaEntry(ap_uint<16> next)
		:next(next) {}
};

struct retransMetaReq
{
	ap_uint<16> idx;
	retransMetaEntry entry;
	bool write;
	bool append;
	retransMetaReq() {}
	retransMetaReq(ap_uint<16> idx)
		:idx(idx), write(false),  append(false) {}
	retransMetaReq(ap_uint<16> idx, ap_uint<16> next)
		:idx(idx), entry(retransMetaEntry(next)), write(false), append(true) {}
	retransMetaReq(ap_uint<16> idx, retransMetaEntry e)
		:idx(idx), entry(e), write(true), append(false) {}
};

struct pointerReq
{
	ap_uint<16>	qpn;
	bool		lock;
	pointerReq() {}
	pointerReq(ap_uint<16> qpn)
		:qpn(qpn), lock(false) {}
	pointerReq(ap_uint<16> qpn, bool l)
		:qpn(qpn), lock(l) {}
};

//TODO maybe introduce seperate request streams
template <int INSTID = 0>
void retrans_pointer_table(	stream<pointerReq>&					pointerReqFifo,
					stream<pointerUpdate>&				pointerUpdFifo,
					stream<retransPointerEntry>& 		pointerRspFifo)
{
#pragma HLS PIPELINE II=1
#pragma HLS INLINE off

	static retransPointerEntry ptr_table[MAX_QPS];
#if defined( __VITIS_HLS__)
	#pragma HLS bind_storage variable=ptr_table type=RAM_T2P impl=BRAM
#else
	#pragma HLS RESOURCE variable=ptr_table core=RAM_T2P_BRAM
#endif

	static ap_uint<16> pt_lockedQP;
	static bool pt_isLocked = false;
	static bool pt_wait = false;
	static pointerReq pt_req;

	pointerUpdate upd;

	if (!pointerUpdFifo.empty())
	{
		pointerUpdFifo.read(upd);
		ptr_table[upd.qpn] = upd.entry;
		if (pt_lockedQP == upd.qpn)
		{
			pt_isLocked = false;
		}
	}
	else if (!pointerReqFifo.empty() && !pt_wait)
	{
		pointerReqFifo.read(pt_req);
		if (pt_req.lock && pt_isLocked)
		{
			pt_wait = true;
		}
		else
		{
			pointerRspFifo.write(ptr_table[pt_req.qpn]);
			if (pt_req.lock)
			{
				pt_isLocked = true;
				pt_lockedQP = pt_req.qpn;
			}
		}
	}
	else if (pt_wait && !pt_isLocked)
	{
		pointerRspFifo.write(ptr_table[pt_req.qpn]);
		pt_isLocked = true;
		pt_lockedQP = pt_req.qpn;
		pt_wait = false;
	}
}

template <int INSTID = 0>
void retrans_meta_table(stream<retransMetaReq>&		meta_upd_req,
						stream<retransMetaEntry>&		meta_rsp)
{
#pragma HLS PIPELINE II=1
#pragma HLS INLINE off

	static retransMetaEntry meta_table[META_TABLE_SIZE];
#if defined( __VITIS_HLS__)
	#pragma HLS bind_storage variable=meta_table type=RAM_T2P impl=BRAM
#else
	#pragma HLS RESOURCE variable=meta_table core=RAM_T2P_BRAM
#endif
	#pragma HLS DEPENDENCE variable=meta_table inter false

	retransMetaReq req;

    if (!meta_upd_req.empty())
    {
        meta_upd_req.read(req);
        if (req.write)
        {
            meta_table[req.idx] = req.entry;
        }
        else if (req.append)
        {
            meta_table[req.idx].next = req.entry.next;
            meta_table[req.idx].isTail = false;
        }
        else
        {
            meta_rsp.write(meta_table[req.idx]);
        }
    }
}

template <int INSTID = 0>
void process_retransmissions(	stream<retransRelease>&	rx2retrans_release_upd,
					stream<retransmission>& rx2retrans_req,
					stream<retransmission>& timer2retrans_req,
					stream<retransEntry>&	tx2retrans_insertRequest,
					stream<pointerReq>&					pointerReqFifo,
					stream<pointerUpdate>&				pointerUpdFifo,
					stream<retransPointerEntry>& 		pointerRspFifo, //TODO reorder
					stream<retransMetaReq>&				metaReqFifo,
					stream<retransMetaEntry>&			metaRspFifo,
					stream<ap_uint<16> >&				freeListFifo,
					stream<ap_uint<16> >&				releaseFifo,
					stream<retransEvent>&				retrans2event)
{
#pragma HLS PIPELINE II=1
#pragma HLS INLINE off

	enum retransStateType {MAIN, INSERT_0, INSERT_1, RELEASE_0, RELEASE_1, RETRANS_0, RETRANS_1, RETRANS_2, TIMER_RETRANS_0, TIMER_RETRANS_1};
	static retransStateType rt_state = MAIN;
	static retransRelease release;
	static ap_uint<16> curr;
	static ap_uint<16> newMetaIdx;
	static retransEntry insert;
	static retransmission retrans;
	static retransMetaEntry meta; //TODO register needed??
	static retransPointerEntry ptrMeta;

	switch (rt_state)
	{
	case MAIN:
		if (!rx2retrans_release_upd.empty())
		{
			rx2retrans_release_upd.read(release);
			pointerReqFifo.write(pointerReq(release.qpn, true));
			rt_state = RELEASE_0;
			std::cout << std::hex << "[PROCESS RETRANSMISSION " << INSTID << "]: releasing " << release.latest_acked_req << std::endl;
		}
		else if (!tx2retrans_insertRequest.empty() && !freeListFifo.empty())
		{
			newMetaIdx = freeListFifo.read();					// check whether we still have place to insert into `retrans meta table`
			tx2retrans_insertRequest.read(insert);
			pointerReqFifo.write(pointerReq(insert.qpn, true));	// enquire whether we have previous req for this qpn
			rt_state = INSERT_0;
			std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: inserting new meta, psn " << std::hex << insert.psn << ", occupying pointer " << newMetaIdx << std::endl;
		}
		else if (!rx2retrans_req.empty())
		{
			rx2retrans_req.read(retrans);
			std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: RX Retransmit triggered!!" << std::endl;
			pointerReqFifo.write(pointerReq(retrans.qpn));
			rt_state = RETRANS_0;
		}
		else if (!timer2retrans_req.empty())
		{
			std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: TIMER Retransmit triggered!!\n";
			timer2retrans_req.read(retrans);
			// Uses always head psn
			pointerReqFifo.write(pointerReq(retrans.qpn)); // enquire whether we have previous req for this qpn
			rt_state = TIMER_RETRANS_0;
		}
		break;
	case INSERT_0:
		if (!pointerRspFifo.empty())
		{
			pointerRspFifo.read(ptrMeta);
			if (!ptrMeta.valid)
			{
				// first request for this qpn, write into both `retrans pointer table`, `retrans meta table`
				ptrMeta.valid = true;
				ptrMeta.head = newMetaIdx;
				ptrMeta.tail = newMetaIdx;
				metaReqFifo.write(retransMetaReq(newMetaIdx, retransMetaEntry(insert)));
				pointerUpdFifo.write(pointerUpdate(insert.qpn, ptrMeta));
				rt_state = MAIN;
				std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: inserting new entry at qpn " << insert.qpn << std::endl;
			}
			else
			{
				// Append new pointer to the tail in `Retrans Meta Table`
				metaReqFifo.write(retransMetaReq(ptrMeta.tail, newMetaIdx));
				ptrMeta.tail = newMetaIdx;
				rt_state = INSERT_1;
				std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: appending entry at qpn " << insert.qpn << std::endl;
			}
		}
		break;
	case INSERT_1:
		// Add new entry into `Retrans Meta Table`
		metaReqFifo.write(retransMetaReq(newMetaIdx, retransMetaEntry(insert)));
		// updates `Retrans Pointer Table` with new "tail"
		pointerUpdFifo.write(pointerUpdate(insert.qpn, ptrMeta));
		rt_state = MAIN;
		break;
	case RELEASE_0:
		if (!pointerRspFifo.empty())
		{
			pointerRspFifo.read(ptrMeta);
			if (ptrMeta.valid)
			{
				// Enquire the first uncleared index in `Retrans Meta Table`
				metaReqFifo.write(retransMetaReq(ptrMeta.head));
				curr = ptrMeta.head;
				rt_state = RELEASE_1;
			}
			else
			{
				std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: state RELEASE_0 invalid meta entry\n";
				// Release lock
				pointerUpdFifo.write(pointerUpdate(release.qpn, ptrMeta));
				rt_state = MAIN;
			}
		}
		break;
	case RELEASE_1:
		if (!metaRspFifo.empty())
		{
			metaRspFifo.read(meta);

			// we should ideally never gets into this condition
			if (meta.psn > release.latest_acked_req || !meta.valid)
			{
				// useless ACK
				std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: state RELEASE_1 invalid state" << std::endl;
				// Release lock
				pointerUpdFifo.write(pointerUpdate(release.qpn, ptrMeta));
				rt_state = MAIN;
				break;
			}
			// released up till RC ACK psn
			else if (meta.psn == release.latest_acked_req)
			{
				// the "next" index is now the head
				ptrMeta.head = meta.next;
				ptrMeta.valid = !meta.isTail;
				pointerUpdFifo.write(pointerUpdate(release.qpn, ptrMeta));
				rt_state = MAIN;
			}
			else if (meta.isTail)
			{
				// this is the last stored req, although psn still smaller than ACK
				ptrMeta.valid = false;
				ptrMeta.head = curr;
				pointerUpdFifo.write(pointerUpdate(release.qpn, ptrMeta));
				rt_state = MAIN;
			}
			else
			{
				// keep traversing the list until meet the conditions
				metaReqFifo.write(retransMetaReq(meta.next));
			}

			// clear `Freelist Handler`
			releaseFifo.write(curr);
			std::cout << std::hex << "[PROCESS RETRANSMISSION " << INSTID << "]: release success, psn " << meta.psn  << ", cleared pointer " << curr << std::endl;

			curr = meta.next;
		}
		break;
	case RETRANS_0:
		if (!pointerRspFifo.empty())
		{
			std::cout << "[PROCESS RETRANSMISSION " << INSTID << "]: NAK, retransmitting qpn " << retrans.qpn << std::endl;
			pointerRspFifo.read(ptrMeta);
			rt_state = MAIN;
			if (ptrMeta.valid)
			{
				// Get continuous stream of meta entries
				metaReqFifo.write(retransMetaReq(ptrMeta.head));
				curr = ptrMeta.head;
				rt_state = RETRANS_1;
			}
		}
		break;
	case RETRANS_1:
		// loop until we get the psn "head" for retransmission
		if (!metaRspFifo.empty())
		{
			metaRspFifo.read(meta);
			rt_state = MAIN;
			if (meta.valid)
			{
				if (!meta.isTail)
				{
					metaReqFifo.write(retransMetaReq(meta.next));
				}
				else
				{
					rt_state = MAIN;
				}

				// check if we should start retransmitting
				if (meta.psn == retrans.psn)
				{
					std::cout << std::hex << "[PROCESS RETRANSMISSION " << INSTID << "]: NAK, retransmitting psn: " << meta.psn << std::endl;
					retrans2event.write(retransEvent(meta.opCode, retrans.qpn, meta.localAddr, meta.remoteAddr, meta.length, meta.psn));
					if (!meta.isTail)
					{
						rt_state = RETRANS_2;
					}
				}
				else
				{
					// keep searching
					rt_state = RETRANS_1;
				}
			}
		}
		break;
	case RETRANS_2:
		//Retransmit everything until we reach tail
		if (!metaRspFifo.empty())
		{
			metaRspFifo.read(meta);
			rt_state = MAIN;
			if (meta.valid)
			{
				if (!meta.isTail)
				{
					// keep retransmitting
					metaReqFifo.write(retransMetaReq(meta.next));
					rt_state = RETRANS_2;
				}
				std::cout << std::hex << "[PROCESS RETRANSMISSION " << INSTID << "]: NAK, retransmitting psn: " << meta.psn << std::endl;
				retrans2event.write(retransEvent(meta.opCode, retrans.qpn, meta.localAddr, meta.remoteAddr, meta.length, meta.psn));
			}
		}
		break;
	case TIMER_RETRANS_0:
		if (!pointerRspFifo.empty())
		{
			std::cout << std::hex << "[PROCESS RETRANSMISSION " << INSTID << "]: timed out, retransmitting qpn " << retrans.qpn << std::endl;
			pointerRspFifo.read(ptrMeta);
			if (ptrMeta.valid)
			{
				// Get continuous stream of meta entries
				metaReqFifo.write(retransMetaReq(ptrMeta.head));
				rt_state = TIMER_RETRANS_1;
			}
			else
			{
				rt_state = MAIN;
			}
		}
		break;
	case TIMER_RETRANS_1:
		if (!metaRspFifo.empty())
		{
			metaRspFifo.read(meta);
			rt_state = MAIN;
			if (meta.valid)
			{
				if (!meta.isTail)
				{
					// keep retransmitting
					metaReqFifo.write(retransMetaReq(meta.next));
					rt_state = TIMER_RETRANS_1;
				}
				std::cout << std::hex << "[PROCESS RETRANSMISSION " << INSTID << "]: timed out, retransmitting psn " << meta.psn << std::endl;
				retrans2event.write(retransEvent(meta.opCode, retrans.qpn, meta.localAddr, meta.remoteAddr, meta.length, meta.psn));
			}
		}
		break;
	}//switch
}

template <int INSTID = 0>
void freelist_handler(	stream<ap_uint<16> >& rt_releaseFifo,
						stream<ap_uint<16> >& rt_freeListFifo)
{
#pragma HLS PIPELINE II=1
#pragma HLS INLINE off

	static ap_uint<16> freeListCounter = 0;
	#pragma HLS reset variable=freeListCounter

	if (!rt_releaseFifo.empty())
	{
		rt_freeListFifo.write(rt_releaseFifo.read());
	}
	// for initialistion to load all pointers into the FIFO
	else if (freeListCounter < META_TABLE_SIZE && !rt_freeListFifo.full())
	{
		rt_freeListFifo.write(freeListCounter);
		freeListCounter++;
	}
}

template <int INSTID = 0>
void retransmitter(	stream<retransRelease>&	rx2retrans_release_upd,
					stream<retransmission>& rx2retrans_req,
					stream<retransmission>& timer2retrans_req,
					stream<retransEntry>&	tx2retrans_insertRequest,
					stream<retransEvent>&	retrans2event)
{
//#pragma HLS DATAFLOW
//#pragma HLS INTERFACE ap_ctrl_none register port=return
#pragma HLS INLINE

	static stream<pointerReq>				rt_pointerReqFifo("rt_pointerReqFifo");
	static stream<pointerUpdate>			rt_pointerUpdFifo("rt_pointerUpdFifo");
	static stream<retransPointerEntry> 		rt_pointerRspFifo("rt_pointerRspFifo"); //TODO reorder
	#pragma HLS STREAM depth=2 variable=rt_pointerReqFifo
	#pragma HLS STREAM depth=2 variable=rt_pointerUpdFifo
	#pragma HLS STREAM depth=2 variable=rt_pointerRspFifo

	static stream<retransMetaReq>				rt_metaReqFifo("rt_metaReqFifo");
	static stream<retransMetaEntry>			rt_metaRspFifo("rt_metaRspFifo");
	#pragma HLS STREAM depth=2 variable=rt_metaReqFifo
	#pragma HLS STREAM depth=2 variable=rt_metaRspFifo

	static stream<ap_uint<16> > rt_freeListFifo("rt_freeListFifo");
	#pragma HLS STREAM depth=META_TABLE_SIZE variable=rt_freeListFifo
	static stream<ap_uint<16> > rt_releaseFifo("rt_releaseFifo");
	#pragma HLS STREAM depth=2 variable=rt_releaseFifo

	freelist_handler<INSTID>(rt_releaseFifo, rt_freeListFifo);

	retrans_pointer_table<INSTID>(rt_pointerReqFifo, rt_pointerUpdFifo, rt_pointerRspFifo);

	retrans_meta_table<INSTID>( rt_metaReqFifo,
						rt_metaRspFifo);

	process_retransmissions<INSTID>(rx2retrans_release_upd,
							rx2retrans_req,
							timer2retrans_req,
							tx2retrans_insertRequest,
							rt_pointerReqFifo,
							rt_pointerUpdFifo,
							rt_pointerRspFifo,
							rt_metaReqFifo,
							rt_metaRspFifo,
							rt_freeListFifo,
							rt_releaseFifo,
							retrans2event);
}
