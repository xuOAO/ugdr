#pragma once
#include <cstdint>

namespace ugdr{
namespace common{

constexpr uint32_t UGDR_MAX_DEV_NAME_LEN = 64;
constexpr uint32_t UGDR_MAX_SHRING_NAME_LEN = UGDR_MAX_DEV_NAME_LEN + 64; //dev_name + "_xq_shring_xxx"
constexpr uint32_t UGDR_MAX_SEND_FDS_NUM = 2;

enum class WrOpcode : uint32_t {
    SEND = 0,
    SEND_WITH_IMM,
    RDMA_WRITE,
    RDMA_WRITE_WITH_IMM,
    RDMA_READ,
    ATOMIC_CMP_AND_SWP,
    ATOMIC_FETCH_AND_ADD,
    RECV,
    RECV_RDMA_WITH_IMM,
};

enum class WcStatus : uint32_t {
    SUCCESS = 0,
    LOC_LEN_ERR,
    LOC_QP_OP_ERR,
    LOC_EEC_OP_ERR,
    LOC_PROT_ERR,
    WR_FLUSH_ERR,
    MW_BIND_ERR,
    BAD_RESP_ERR,
    LOC_ACCESS_ERR,
    REM_INV_REQ_ERR,
    REM_ACCESS_ERR,
    REM_OP_ERR,
    RETRY_EXC_ERR,
    RNR_RETRY_EXC_ERR,
    LOC_RDD_VIOL_ERR,
    REM_INV_RD_REQ_ERR,
    REM_ABORT_ERR,
    INV_EECN_ERR,
    INV_EEC_STATE_ERR,
    FATAL_ERR,
    RESP_TIMEOUT_ERR,
    GENERAL_ERR
};

enum SendFlags {
    FENCE = 1 << 0,
    SIGNALED = 1 << 1,
    SOLICITED = 1 << 2,
    INLINE = 1 << 3,
};

struct Sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct alignas(64) Wqe {
    uint64_t wr_id;
    WrOpcode opcode; // WrOpcode
    uint32_t flags;  // SendFlags
    // --- Operation Specific (24 Bytes Max) ---
    union {
        // For RDMA Write / Read
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
            uint32_t reserved; // Padding
        } rdma;

        // For Send / Recv (Send with Imm)
        struct {
            uint32_t remote_qpn; // For UD
            uint32_t remote_qkey;// For UD
            uint32_t imm_data;
        } send;

        // // For Atomic
        // struct {
        //     uint64_t remote_addr;
        //     uint64_t compare; // or add value
        //     uint64_t swap;
        //     uint32_t rkey;
        // } atomic; // 注意：Atomic可能会撑大结构体，Slice3暂时可以不放
    };
    
    Sge sge; // Simplified: Support 1 SGE inline
    uint32_t qp_num; // Optional: for verification
};

struct alignas(64) Cqe {
    uint64_t wr_id;
    WcStatus status;
    uint32_t byte_len;
    uint32_t qp_num;
    
    // 以下字段不是必须的，看需求决定是否保留以压缩到 32B
    // Opcode 有时候不需要回传，因为 User 知道 wr_id 对应的 Opcode
    WrOpcode opcode; 
    uint32_t imm_data; 
    
    uint32_t src_qp;   // For UD/Recv
    uint32_t wc_flags; // e.g., GRH present
};

}
}
