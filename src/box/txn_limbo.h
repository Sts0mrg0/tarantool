#pragma once
/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "small/rlist.h"
#include "vclock.h"

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct txn;

/**
 * Transaction and its quorum metadata, to be stored in limbo.
 */
struct txn_limbo_entry {
	/** Link for limbo's queue. */
	struct rlist in_queue;
	/** Transaction, waiting for a quorum. */
	struct txn *txn;
	/**
	 * LSN of the transaction by the originator's vclock
	 * component. May be -1 in case the transaction is not
	 * written to WAL yet.
	 */
	int64_t lsn;
	/**
	 * Number of ACKs. Or in other words - how many replicas
	 * confirmed receipt of the transaction.
	 */
	int ack_count;
	/**
	 * Result flags. Only one of them can be true. But both
	 * can be false if the transaction is still waiting for
	 * its resolution.
	 */
	bool is_commit;
	bool is_rollback;
};

static inline bool
txn_limbo_entry_is_complete(const struct txn_limbo_entry *e)
{
	return e->is_commit || e->is_rollback;
}

/**
 * Limbo is a place where transactions are stored, which are
 * finished, but not committed nor rolled back. These are
 * synchronous transactions in progress of collecting ACKs from
 * replicas.
 * Limbo's main purposes
 *   - maintain the transactions ordered by LSN of their emitter;
 *   - be a link between transaction and replication modules, so
 *     as they wouldn't depend on each other directly.
 */
struct txn_limbo {
	/**
	 * Queue of limbo entries. Ordered by LSN. Some of the
	 * entries in the end may not have an LSN yet (their local
	 * WAL write is still in progress), but their order won't
	 * change anyway. Because WAL write completions will give
	 * them LSNs in the same order.
	 */
	struct rlist queue;
	/**
	 * Instance ID of the owner of all the transactions in the
	 * queue. Strictly speaking, nothing prevents to store not
	 * own transactions here, originated from some other
	 * instance. But still the queue may contain only
	 * transactions of the same instance. Otherwise LSN order
	 * won't make sense - different nodes have own independent
	 * LSNs in their vclock components.
	 */
	uint32_t instance_id;
	/**
	 * Condition to wait for completion. It is supposed to be
	 * signaled when the synchro parameters change. Allowing
	 * the sleeping fibers to reconsider their timeouts when
	 * the parameters are updated.
	 */
	struct fiber_cond wait_cond;
	/**
	 * All components of the vclock are versions of the limbo
	 * owner's LSN, how it is visible on other nodes. For
	 * example, assume instance ID of the limbo is 1. Then
	 * vclock[1] here is local LSN of the instance 1.
	 * vclock[2] is how replica with ID 2 sees LSN of
	 * instance 1.
	 * vclock[3] is how replica with ID 3 sees LSN of
	 * instance 1, and so on.
	 * In that way by looking at this vclock it is always can
	 * be said up to which LSN there is a sync quorum for
	 * transactions, created on the limbo's owner node.
	 */
	struct vclock vclock;
	/**
	 * Total number of performed rollbacks. It used as a guard
	 * to do some actions assuming all limbo transactions will
	 * be confirmed, and to check that there were no rollbacks
	 * in the end.
	 */
	int64_t rollback_count;
	/**
	 * Whether the limbo is in rollback mode. The meaning is exactly the
	 * same as for the similar WAL flag. In theory this should be deleted
	 * if the limbo will be ever moved to WAL thread. It would reuse the WAL
	 * flag.
	 * It is a sign to immediately rollback all new limbo entries, if there
	 * is an existing rollback in progress. This technique is called
	 * 'cascading rollback'. Cascading rollback does not allow to write to
	 * WAL anything new so as not to violate the 'reversed rollback order'
	 * rule.
	 * Without cascading rollback it could happen, that the limbo would
	 * start writing ROLLBACK to WAL, then a new transaction would be added
	 * to limbo and sent to WAL too. In the result the new transaction would
	 * be stored in WAL after ROLLBACK, and yet it should be rolled back too
	 * by the 'reversed rollback order' rule - contradiction.
	 */
	bool is_in_rollback;
};

/**
 * Global limbo entry. So far an instance can have only one limbo,
 * where master's transactions are stored. Eventually there may
 * appear more than one limbo for master-master support.
 */
extern struct txn_limbo txn_limbo;

static inline bool
txn_limbo_is_empty(struct txn_limbo *limbo)
{
	return rlist_empty(&limbo->queue);
}

static inline struct txn_limbo_entry *
txn_limbo_first_entry(struct txn_limbo *limbo)
{
	return rlist_first_entry(&limbo->queue, struct txn_limbo_entry,
				 in_queue);
}

static inline struct txn_limbo_entry *
txn_limbo_last_entry(struct txn_limbo *limbo)
{
	return rlist_last_entry(&limbo->queue, struct txn_limbo_entry,
				in_queue);
}

/**
 * Allocate, create, and append a new transaction to the limbo.
 * The limbo entry is allocated on the transaction's region.
 */
struct txn_limbo_entry *
txn_limbo_append(struct txn_limbo *limbo, uint32_t id, struct txn *txn);

/** Remove the entry from the limbo, mark as rolled back. */
void
txn_limbo_abort(struct txn_limbo *limbo, struct txn_limbo_entry *entry);

/**
 * Assign a remote LSN to a limbo entry. That happens when a
 * remote transaction is added to the limbo and starts waiting for
 * a confirm.
 */
void
txn_limbo_assign_remote_lsn(struct txn_limbo *limbo,
			    struct txn_limbo_entry *entry, int64_t lsn);

/**
 * Assign a local LSN to a limbo entry. That happens when a local
 * transaction is written to WAL.
 */
void
txn_limbo_assign_local_lsn(struct txn_limbo *limbo,
			   struct txn_limbo_entry *entry, int64_t lsn);

/**
 * Assign an LSN to a limbo entry. Works both with local and
 * remote transactions. The function exists to be used in a
 * context, where a transaction is not known whether it is local
 * or not. For example, when a transaction is committed not bound
 * to any fiber (txn_commit_async()), it can be created by applier
 * (then it is remote) or by recovery (then it is local). Besides,
 * recovery can commit remote transactions as well, when works on
 * a replica - it will recover data received from master.
 */
void
txn_limbo_assign_lsn(struct txn_limbo *limbo, struct txn_limbo_entry *entry,
		     int64_t lsn);

/**
 * Ack all transactions up to the given LSN on behalf of the
 * replica with the specified ID.
 */
void
txn_limbo_ack(struct txn_limbo *limbo, uint32_t replica_id, int64_t lsn);

/**
 * Block the current fiber until the transaction in the limbo
 * entry is either committed or rolled back.
 * If timeout is reached before acks are collected, the tx is
 * rolled back as well as all the txs in the limbo following it.
 * Returns -1 when rollback was performed and tx has to be freed.
 *          0 when tx processing can go on.
 */
int
txn_limbo_wait_complete(struct txn_limbo *limbo, struct txn_limbo_entry *entry);

/**
 * Confirm all the entries up to the given master's LSN.
 */
void
txn_limbo_read_confirm(struct txn_limbo *limbo, int64_t lsn);

/**
 * Rollback all the entries  starting with given master's LSN.
 */
void
txn_limbo_read_rollback(struct txn_limbo *limbo, int64_t lsn);

/**
 * Waiting for confirmation of all "sync" transactions
 * during confirm timeout or fail.
 */
int
txn_limbo_wait_confirm(struct txn_limbo *limbo);

/**
 * Make txn_limbo confirm all the entries with lsn less than or
 * equal to the given one, and rollback all the following entries.
 * The function makes txn_limbo write CONFIRM and ROLLBACK
 * messages for appropriate lsns, and then process the messages
 * immediately.
 */
void
txn_limbo_force_empty(struct txn_limbo *limbo, int64_t last_confirm);

/**
 * Update qsync parameters dynamically.
 */
void
txn_limbo_on_parameters_change(struct txn_limbo *limbo);

/**
 * Initialize qsync engine.
 */
void
txn_limbo_init();

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */
