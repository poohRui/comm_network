#include <infiniband/verbs.h>
#include "comm_network/ibverbs_qp.h"
#include "comm_network/message.h"
#include "comm_network/ibverbs_comm_network.h"

namespace comm_network {
IBVerbsQP::IBVerbsQP(ibv_context* ctx, ibv_pd* pd, ibv_cq* send_cq, ibv_cq* recv_cq) {
  // ctx_, pd_
  ctx_ = ctx;
  pd_ = pd;
  // qp_
  ibv_device_attr device_attr;
  CHECK_EQ(ibv_query_device(ctx, &device_attr), 0);
  uint32_t max_recv_wr = rdma_recv_msg_buf_byte / sizeof(Msg);
  max_recv_wr = std::min<uint32_t>(max_recv_wr, device_attr.max_qp_wr);
  ibv_qp_init_attr qp_init_attr;
  qp_init_attr.qp_context = nullptr;
  qp_init_attr.send_cq = send_cq;
  qp_init_attr.recv_cq = recv_cq;
  qp_init_attr.srq = nullptr;
  qp_init_attr.cap.max_send_wr = device_attr.max_qp_wr;
  qp_init_attr.cap.max_recv_wr = max_recv_wr;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_sge = 1;
  qp_init_attr.cap.max_inline_data = 0;
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.sq_sig_all = 1;
  qp_ = ibv_create_qp(pd, &qp_init_attr);
  CHECK(qp_);
  // recv_msg_buf_
  recv_msg_buf_.assign(max_recv_wr, nullptr);
  FOR_RANGE(size_t, i, 0, recv_msg_buf_.size()) { recv_msg_buf_.at(i) = new MsgMR(pd_); }
  // send_msg_buf_
  CHECK(send_msg_buf_.empty());
}

IBVerbsQP::~IBVerbsQP() {
  CHECK_EQ(ibv_destroy_qp(qp_), 0);
  while (send_msg_buf_.empty() == false) {
    delete send_msg_buf_.front();
    send_msg_buf_.pop();
  }
  for (MsgMR* msg_mr : recv_msg_buf_) { delete msg_mr; }
}

void IBVerbsQP::Connect(const IBVerbsConnectionInfo& peer_info) {
  ibv_port_attr port_attr;
  CHECK_EQ(ibv_query_port(ctx_, 1, &port_attr), 0);
  ibv_qp_attr qp_attr;
  // IBV_QPS_INIT
  memset(&qp_attr, 0, sizeof(ibv_qp_attr));
  qp_attr.qp_state = IBV_QPS_INIT;
  qp_attr.pkey_index = 0;
  qp_attr.port_num = 1;
  qp_attr.qp_access_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
  CHECK_EQ(ibv_modify_qp(qp_, &qp_attr,
                         IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS),
           0);
  // IBV_QPS_RTR
  memset(&qp_attr, 0, sizeof(ibv_qp_attr));
  qp_attr.qp_state = IBV_QPS_RTR;
  qp_attr.ah_attr.grh.dgid.global.subnet_prefix = peer_info.subnet_prefix();
  qp_attr.ah_attr.grh.dgid.global.interface_id = peer_info.interface_id();
  qp_attr.ah_attr.grh.flow_label = 0;
  qp_attr.ah_attr.grh.sgid_index = 0;
  qp_attr.ah_attr.grh.hop_limit = std::numeric_limits<uint8_t>::max();
  qp_attr.ah_attr.dlid = peer_info.lid();
  qp_attr.ah_attr.sl = 0;
  qp_attr.ah_attr.src_path_bits = 0;
  qp_attr.ah_attr.static_rate = 0;
  qp_attr.ah_attr.is_global = 1;
  qp_attr.ah_attr.port_num = 1;
  qp_attr.path_mtu = port_attr.active_mtu;
  qp_attr.dest_qp_num = peer_info.qp_num();
  qp_attr.rq_psn = 0;
  qp_attr.max_dest_rd_atomic = 1;
  qp_attr.min_rnr_timer = 12;
  CHECK_EQ(ibv_modify_qp(qp_, &qp_attr,
                         IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN
                             | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER),
           0);
  // IBV_QPS_RTS
  memset(&qp_attr, 0, sizeof(ibv_qp_attr));
  qp_attr.qp_state = IBV_QPS_RTS;
  qp_attr.sq_psn = 0;
  qp_attr.max_rd_atomic = 1;
  qp_attr.retry_cnt = 7;
  qp_attr.rnr_retry = 7;
  qp_attr.timeout = 14;
  CHECK_EQ(ibv_modify_qp(qp_, &qp_attr,
                         IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_RETRY_CNT
                             | IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT),

           0);
}

void IBVerbsQP::PostAllRecvRequest() {
  for (MsgMR* msg_mr : recv_msg_buf_) { PostRecvRequest(msg_mr); }
}

void IBVerbsQP::PostWriteRequest(const IBVerbsMemDescProto& remote_mem,
                                 const IBVerbsMemDesc& local_mem, const Msg& write_partial) {
  CHECK_EQ(remote_mem.mem_ptr_size(), local_mem.sge_vec().size());
  WorkRequestId* wr_id = NewWorkRequestId();
  wr_id->outstanding_sge_cnt = local_mem.sge_vec().size();
  wr_id->msg_write_partial = write_partial;
  FOR_RANGE(size_t, i, 0, local_mem.sge_vec().size()) {
    ibv_send_wr wr;
    wr.wr_id = reinterpret_cast<uint64_t>(wr_id);
    wr.next = nullptr;
    wr.sg_list = const_cast<ibv_sge*>(&(local_mem.sge_vec().at(i)));
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = 0;
    wr.imm_data = 0;
    wr.wr.rdma.remote_addr = remote_mem.mem_ptr(i);
    wr.wr.rdma.rkey = remote_mem.mr_rkey(i);
    ibv_send_wr* bad_wr = nullptr;
    CHECK_EQ(ibv_post_send(qp_, &wr, &bad_wr), 0);
  }
}

void IBVerbsQP::PostSendRequest(const Msg& msg) {
  MsgMR* msg_mr = GetOneSendMsgMRFromBuf();
  msg_mr->set_msg(msg);
  WorkRequestId* wr_id = NewWorkRequestId();
  wr_id->msg_mr = msg_mr;
  ibv_send_wr wr;
  wr.wr_id = reinterpret_cast<uint64_t>(wr_id);
  wr.next = nullptr;
  wr.sg_list = const_cast<ibv_sge*>(&(msg_mr->mem_desc().sge_vec().at(0)));
  wr.num_sge = 1;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = 0;
  wr.imm_data = 0;
  memset(&(wr.wr), 0, sizeof(wr.wr));
  ibv_send_wr* bad_wr = nullptr;
  CHECK_EQ(ibv_post_send(qp_, &wr, &bad_wr), 0);
}

void IBVerbsQP::WriteDone(WorkRequestId* wr_id) {
  Msg write_partial = wr_id->msg_write_partial;
  Msg msg;
  msg.msg_type = MsgType::kPartialWriteDone;
  LOG(INFO) << "Write Done " << write_partial.msg_body.buffer_id;
  msg.msg_body.data_size = write_partial.msg_body.data_size;
  msg.msg_body.src_machine_id = write_partial.msg_body.src_machine_id;
  msg.msg_body.dst_machine_id = write_partial.msg_body.dst_machine_id;
  msg.msg_body.buffer_id = write_partial.msg_body.buffer_id;
  msg.msg_body.dst_addr = write_partial.msg_body.dst_addr;
  msg.msg_body.piece_id = write_partial.msg_body.piece_id;
  msg.msg_body.num_of_pieces = write_partial.msg_body.num_of_pieces;
  Global<IBVerbsCommNet>::Get()->SendMsg(write_partial.msg_body.dst_machine_id, msg);
  DeleteWorkRequestId(wr_id);
}

void IBVerbsQP::SendDone(WorkRequestId* wr_id) {
  {
    std::unique_lock<std::mutex> lck(send_msg_buf_mtx_);
    send_msg_buf_.push(wr_id->msg_mr);
  }
  DeleteWorkRequestId(wr_id);
}

void IBVerbsQP::RecvDone(WorkRequestId* wr_id, Channel<Msg>* msg_channel) {
  Msg recv_msg = wr_id->msg_mr->msg();
  switch (recv_msg.msg_type) {
    case (MsgType::kDataIsReady): {
      Msg msg;
      msg.msg_type = MsgType::kAllocateMemory;
      msg.msg_body.data_size = recv_msg.msg_body.data_size;
      msg.msg_body.src_addr = recv_msg.msg_body.src_addr;
      msg.msg_body.src_machine_id = recv_msg.msg_body.src_machine_id;
      msg_channel->Send(msg);
      break;
    }
    case (MsgType::kPleaseWrite): {
      Msg msg;
      msg.msg_type = MsgType::kDoWrite;
      msg.msg_body.src_addr = recv_msg.msg_body.src_addr;
      msg.msg_body.dst_addr = recv_msg.msg_body.dst_addr;
      msg.msg_body.data_size = recv_msg.msg_body.data_size;
      msg.msg_body.src_machine_id = recv_msg.msg_body.src_machine_id;
      msg.msg_body.dst_machine_id = recv_msg.msg_body.dst_machine_id;
      msg_channel->Send(msg);
      break;
    }
    case (MsgType::kPartialWriteDone): {
      Global<IBVerbsCommNet>::Get()->Register2NormalMemory(recv_msg);
      break;
    }
    case (MsgType::kFreeBufferPair): {
      int64_t src_machine_id = recv_msg.msg_body.src_machine_id;
      int64_t dst_machine_id = recv_msg.msg_body.dst_machine_id;
      int8_t buffer_id = recv_msg.msg_body.buffer_id;
      Global<IBVerbsCommNet>::Get()->ReleaseBuffer(src_machine_id, dst_machine_id, buffer_id);
      break;
    }
    default: {
      LOG(INFO) << "Unsupport receive message type";
      break;
    }
  }
  PostRecvRequest(wr_id->msg_mr);
  DeleteWorkRequestId(wr_id);
}

void IBVerbsQP::PostRecvRequest(MsgMR* msg_mr) {
  WorkRequestId* wr_id = NewWorkRequestId();
  wr_id->msg_mr = msg_mr;
  ibv_recv_wr wr;
  wr.wr_id = reinterpret_cast<uint64_t>(wr_id);
  wr.next = nullptr;
  wr.sg_list = const_cast<ibv_sge*>(&(msg_mr->mem_desc().sge_vec().at(0)));
  wr.num_sge = 1;
  ibv_recv_wr* bad_wr = nullptr;
  CHECK_EQ(ibv_post_recv(qp_, &wr, &bad_wr), 0);
}

MsgMR* IBVerbsQP::GetOneSendMsgMRFromBuf() {
  std::unique_lock<std::mutex> lck(send_msg_buf_mtx_);
  if (send_msg_buf_.empty()) { send_msg_buf_.push(new MsgMR(pd_)); }
  MsgMR* msg_mr = send_msg_buf_.front();
  send_msg_buf_.pop();
  return msg_mr;
}

WorkRequestId* IBVerbsQP::NewWorkRequestId() {
  WorkRequestId* wr_id = new WorkRequestId;
  wr_id->qp = this;
  wr_id->outstanding_sge_cnt = 0;
  wr_id->msg_mr = nullptr;
  return wr_id;
}

void IBVerbsQP::DeleteWorkRequestId(WorkRequestId* wr_id) {
  CHECK_EQ(wr_id->qp, this);
  delete wr_id;
}
}  // namespace comm_network
