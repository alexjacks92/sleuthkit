/*
 * The Sleuth Kit
 *
 * Brian Carrier [carrier <at> sleuthkit [dot] org]
 * Copyright (c) 2003-2014 Brian Carrier.  All rights reserved
 *
 *
 * This software is distributed under the Common Public License 1.0
 *
 */

#include "tsk_hashdb_i.h"
#include "sqlite_index.h"
#include <assert.h>

// RJCTODO: Name this file consistently...
/**
 * \file sqlite_index.cpp
 * Contains functions for creating a SQLite format hash index // RJCTODO: CHange the name of this file and this comment
 */

static const int chunkSize = 1024 * 1024;
static sqlite3_stmt *m_stmt = NULL; // RJCTODO: Get rid of the m_
static bool need_SQL_index = false; // RJCTODO: Get rid of this
static const char hex[] = "0123456789abcdef";
uint8_t sqlite_v1_addentry_bin(TSK_HDB_INFO * hdb_info, uint8_t* hvalue, int hlen, TSK_OFF_T offset);
uint8_t addentry_text(TSK_HDB_INFO * hdb_info, char* hvalue, TSK_OFF_T offset);
int8_t lookup_text(TSK_HDB_INFO * hdb_info, const char* hvalue, TSK_HDB_FLAG_ENUM flags, TSK_HDB_LOOKUP_FN action, void *ptr);

static int attempt(int resultCode, int expectedResultCode,
		const char *errfmt, sqlite3 * sqlite)
{
	if (resultCode != expectedResultCode) {
		tsk_error_reset();
		tsk_error_set_errno(TSK_ERR_AUTO_DB);
		tsk_error_set_errstr(errfmt, sqlite3_errmsg(sqlite), resultCode);
		return 1;
	}
	return 0;
}

static int attempt_exec(const char *sql, int (*callback) (void *, int, char **, char **),
						void *callback_arg, const char *errfmt, sqlite3 * sqlite)
{
	char * errmsg;

	if(sqlite3_exec(sqlite, sql, callback, callback_arg, &errmsg) != SQLITE_OK) {
		tsk_error_reset();
		tsk_error_set_errno(TSK_ERR_AUTO_DB);
		tsk_error_set_errstr(errfmt, errmsg);
		sqlite3_free(errmsg);
		return 1;
	}
	return 0;
}

static int attempt_exec_nocallback(const char *sql, const char *errfmt, sqlite3 * sqlite)
{
	return attempt_exec(sql, NULL, NULL, errfmt, sqlite);
}

static int finalize_stmt(sqlite3_stmt * stmt)
{
	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		tsk_error_reset();
		tsk_error_set_errno(TSK_ERR_AUTO_DB);
		tsk_error_set_errstr("Error finalizing SQL statement\n");
        tsk_error_print(stderr);
		return 1;
	}
	return 0;
}

static int prepare_stmt(const char *sql, sqlite3_stmt ** ppStmt, sqlite3 * sqlite)
{
    ///@todo possible performance increase by using strlen(sql)+1 instead of -1 // RJCTODO: Resolve this
	if (sqlite3_prepare_v2(sqlite, sql, -1, ppStmt, NULL) != SQLITE_OK) {
		tsk_error_reset();
		tsk_error_set_errno(TSK_ERR_AUTO_DB);
		tsk_error_set_errstr("Error preparing SQL statement: %s\n", sql);
        tsk_error_print(stderr);
		return 1;
	}
	return 0;
}

// RJCTODO: Probably don't need this, since not preserving SQLite index. On the other hand, the additions to the database are transacted,
// and since multiple tables are potentially involved in the operation, the TX may be a good idea.
static uint8_t begin_transaction(TSK_HDB_INFO *hdb_info) {
    TSK_SQLITE_HDB_INFO *sqlite_hdb_info = (TSK_SQLITE_HDB_INFO*)hdb_info;
	return attempt_exec_nocallback("BEGIN", "Error beginning transaction %s\n", sqlite_hdb_info->db);
}

// RJCTODO: Probably don't need this, since not preserving SQLite index. On the other hand, the additions to the database are transacted,
// and since multiple tables are potentially involved in the operation, the TX may be a good idea.
static uint8_t end_transaction(TSK_HDB_INFO *hdb_info) {
    TSK_SQLITE_HDB_INFO *sqlite_hdb_info = (TSK_SQLITE_HDB_INFO*)hdb_info;
	return attempt_exec_nocallback("COMMIT", "Error committing transaction %s\n", sqlite_hdb_info->db);
}

sqlite3 *sqlite_hdb_open_db(TSK_TCHAR *db_file_path)
{
    sqlite3 *db = NULL;
    int opened = 1;
#ifdef TSK_WIN32
    opened = attempt(sqlite3_open16(db_file_path, &db), SQLITE_OK, "Can't open hash database: %s\n", db);
#else
    opened = attempt(sqlite3_open(db_file_path, &db), SQLITE_OK, "Can't open hash database: %s\n", db);
#endif
    if (1 != opened) {
	    sqlite3_extended_result_codes(db, 1);
		attempt_exec_nocallback("PRAGMA synchronous = OFF;", "Error setting PRAGMA synchronous: %s\n", db);
		attempt_exec_nocallback("PRAGMA encoding = \"UTF-8\";", "Error setting PRAGMA encoding UTF-8: %s\n", db);
		attempt_exec_nocallback("PRAGMA read_uncommitted = True;", "Error setting PRAGMA read_uncommitted: %s\n", db);
		attempt_exec_nocallback("PRAGMA page_size = 4096;", "Error setting PRAGMA page_size: %s\n", db);
    }
    else {
        sqlite3_close(db);
        db = NULL;
    }
    return db;
}

// RJCTODO: Comment
uint8_t sqlite_hdb_create_db(TSK_TCHAR *db_file_path)
{
	sqlite3 *db = sqlite_hdb_open_db(db_file_path);
	if (NULL == db) {
		return 1;
	}

    // Incrementally increase the size if the database.    
    if (sqlite3_file_control(db, NULL, SQLITE_FCNTL_CHUNK_SIZE, const_cast<int *>(&chunkSize)) != SQLITE_OK) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_AUTO_DB);
        tsk_error_set_errstr("sqlite_v1_initialize: error setting chunk size %s", sqlite3_errmsg(db));
        return 1;
    }


	if (attempt_exec_nocallback("CREATE TABLE db_properties (name TEXT NOT NULL, value TEXT);", "Error creating db_properties table %s\n", db)) {
		return 1;
	}

	char stmt[1024];
	snprintf(stmt, 1024, "INSERT INTO db_properties (name, value) VALUES ('%s', '%s');", IDX_SCHEMA_VER, IDX_VERSION_NUM);
	if (attempt_exec_nocallback(stmt, "Error adding schema info to db_properties: %s\n", db)) {
		return 1;
	}

	if (attempt_exec_nocallback ("CREATE TABLE hashes (id INTEGER PRIMARY KEY AUTOINCREMENT, md5 BINARY(16) UNIQUE, sha1 BINARY(20), sha2_256 BINARY(32), database_offset INTEGER);", "Error creating hashes table %s\n", db)) {
		return 1;
	}

	if (attempt_exec_nocallback("CREATE TABLE file_names (name TEXT NOT NULL, hash_id INTEGER NOT NULL);", "Error creating file_names table %s\n", db)) {
		return 1;
	}

	if (attempt_exec_nocallback("CREATE TABLE comments (comment TEXT NOT NULL, hash_id INTEGER NOT NULL);", "Error creating comments table %s\n", db)) {
		return 1;
	}

	if (attempt_exec_nocallback("CREATE INDEX md5_index ON hashes(md5);", "Error creating md5_index on md5: %s\n", db)) {
		return 1;
	}
	
	if (attempt_exec_nocallback("CREATE INDEX sha1_index ON hashes(sha1);", "Error creating sha1_index on sha1: %s\n", db)) {
		return 1;
    }

    sqlite3_close(db);

	return 0;
}

/** Init prepared statements. Call before adding to the database. Call finalize() when done.
 *
 * @param hdb_info Hash database state structure
 *
 * @return 1 on error and 0 on success
 *
 */
// RJCTODO: Is this the source of the problem with closing the database on finalize() in Autopsy? 
// Are there prepared statements that are not finalized?
// Why are the statements prepared on every round? This should be done on open, and finalization should happen on close.
// This code is not really needed for index building now, but it is still probably worthwhile for add operations.
//uint8_t
//sqlite_v1_begin(TSK_HDB_INFO * hdb_info)
//{
//    TSK_SQLITE_HDB_INFO *sqlite_hdb_info = (TSK_SQLITE_HDB_INFO*)hdb_info;
//	char * insertStmt;
//
//	if (hdb_info->hash_type == TSK_HDB_HTYPE_MD5_ID) {
//		insertStmt = "INSERT INTO hashes (md5, database_offset) VALUES (?, ?)";
//	} else if (hdb_info->hash_type == TSK_HDB_HTYPE_SHA1_ID) {
//		insertStmt = "INSERT INTO hashes (sha1, database_offset) VALUES (?, ?)";
//	} else {
//        return 1;
//    }
//
//	prepare_stmt(insertStmt, &m_stmt, sqlite_hdb_info->db);
//
//	if (begin_transaction(hdb_info)) {
//		return 1;
//	} else {
//        return 0;
//    }
//}

/**
 * This function is a no-op for SQLite hash database. The index is "internal" to the RDBMS.
 * @return 1 on error and 0 on success
 */
uint8_t sqlite_hdb_make_index(TSK_HDB_INFO * hdb_info, TSK_TCHAR * htype)
{
    // RJCTODO: Add should not be called error stuff
    return 0;
}

// RJCTODO: Comment
// RJCTODO: Get this implementation right. This is the new add function. Build it from:
// tsk_hdb_lookup_str_id(), sqlite_v1_addentry(), sqlite_v1_addentry_bin(), addentry_text(),  
uint8_t
sqlite_hdb_add(TSK_HDB_INFO * hdb_info, const char * filename, const char * md5, const char * sha1, const char * sha256, const char * comment)
{
    return 0;
}

/**
 * This function is a no-op for SQLite hash database. The index is "internal" to the RDBMS.
 * @return 1 on error and 0 on success
 */
//uint8_t
//sqlite_v1_addentry(TSK_HDB_INFO * hdb_info, char* hvalue,
//                    TSK_OFF_T offset)
//{
// RJCTODO: This needs to go into a separate add to hash database function, as opposed to an add to index function
//    hdb_info->idx_info->idx_struct.idx_sqlite_v1->lastId = 0;
//
//	if (strlen(hvalue) != hdb_info->hash_len) {
//		tsk_error_reset();
//		tsk_error_set_errno(TSK_ERR_AUTO_DB);
//		tsk_error_set_errstr("Hash length doesn't match index type: %s\n", hvalue);
//        tsk_error_print(stderr);
//		return 1;
//	}
//
//#ifdef IDX_SQLITE_STORE_TEXT
//    uint8_t ret = addentry_text(hdb_info, hvalue, offset);
//#else
//	const size_t len = (hdb_info->hash_len)/2;
//    uint8_t* hash = (uint8_t*) tsk_malloc(len+1);
//    
//	size_t count;
//
//    // We use an intermediate short to be compatible with Microsoft's implementation of the scanf family format
//    short unsigned int binval;
//    for (count = 0; count < len; count++) {
//		int r = sscanf(hvalue, "%2hx", &binval);
//        hash[count] = (uint8_t) binval;
//		hvalue += 2 * sizeof(char);
//	}
//    uint8_t ret = sqlite_v1_addentry_bin(hdb_info, hash, len, offset);
//
//    delete [] hash;
//#endif
//
//    if (ret == 0) {
//        // The current id can be used by subsequent add name or add comment operations
//	    hdb_info->idx_info->idx_struct.idx_sqlite_v1->lastId = sqlite3_last_insert_rowid(hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite);
//    }
//
//    return ret;
//}

/**
 * This function is a no-op for SQLite hash database. The index is "internal" to the RDBMS.
 * @return 1 on error and 0 on success
 */
//uint8_t
//sqlite_v1_addentry_bin(TSK_HDB_INFO * hdb_info, uint8_t* hvalue, int hlen,
//                    TSK_OFF_T offset)
//{
// RJCTODO: This needs to go into a separate add to hash database function, as opposed to an add to index function
 //   if (attempt(sqlite3_bind_blob(m_stmt, 1, hvalue, hlen, SQLITE_TRANSIENT),
	//	SQLITE_OK,
	//	"Error binding binary blob: %s\n",
	//	hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite) ||
	//	attempt(sqlite3_bind_int64(m_stmt, 2, offset),
	//	SQLITE_OK,
	//	"Error binding entry offset: %s\n",
	//	hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite) ) {
 //       return 1;
 //   }

 //   // Don't report error on constraint -- we just will silently not add that duplicate hash
	//int r = sqlite3_step(m_stmt);
 //   if ((r != SQLITE_DONE) && (r != SQLITE_CONSTRAINT) ) {
	//	tsk_error_reset();
	//	tsk_error_set_errno(TSK_ERR_AUTO_DB);
	//	tsk_error_set_errstr("Error stepping: %s\n", sqlite3_errmsg( hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite), r);
 //       return 1;
 //   }

	//r = sqlite3_reset(m_stmt);
 //   if ((r != SQLITE_OK) && (r != SQLITE_CONSTRAINT) ) {
	//	tsk_error_reset();
	//	tsk_error_set_errno(TSK_ERR_AUTO_DB);
	//	tsk_error_set_errstr("Error resetting: %s\n", sqlite3_errmsg( hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite), r);
 //       return 1;
 //   }

 //   return 0;
//    return 0;
//}

/**
 * This function is a no-op for SQLite hash database. The index is "internal" to the RDBMS.
 * @return 1 on error and 0 on success
 */
//uint8_t
//addentry_text(TSK_HDB_INFO * hdb_info, char* hvalue, TSK_OFF_T offset)
//{
// RJCTODO: This needs to go into a separate add to hash database function, as opposed to an add to index function
 //   if (attempt(sqlite3_bind_text(m_stmt, 1, hvalue, strlen(hvalue), SQLITE_TRANSIENT),
	//	SQLITE_OK,
	//	"Error binding text: %s\n",
	//	hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite) ||
	//	attempt(sqlite3_bind_int64(m_stmt, 2, offset),
	//	    SQLITE_OK,
	//	    "Error binding entry offset: %s\n",
	//	    hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite) ) {
 //       return 1;
 //   }

 //   // Don't report error on constraint -- we just will silently not add that duplicate hash
	//int r = sqlite3_step(m_stmt);
 //   if ((r != SQLITE_DONE) && (r != SQLITE_CONSTRAINT) ) {
	//	tsk_error_reset();
	//	tsk_error_set_errno(TSK_ERR_AUTO_DB);
	//	tsk_error_set_errstr("Error stepping: %s\n", sqlite3_errmsg( hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite), r);
 //       return 1;
 //   }

	//r = sqlite3_reset(m_stmt);
 //   if ((r != SQLITE_OK) && (r != SQLITE_CONSTRAINT) ) {
	//	tsk_error_reset();
	//	tsk_error_set_errno(TSK_ERR_AUTO_DB);
	//	tsk_error_set_errstr("Error resetting: %s\n", sqlite3_errmsg( hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite), r);
 //       return 1;
 //   }

 //   return 0;
//    return 0;
//}

/**
 * Add new comment (e.g. the case name)
 *
 * @param hdb_info Hash database state info structure.
 * @return 1 on error and 0 on success
 */
uint8_t
sqlite_v1_addcomment(TSK_HDB_INFO * hdb_info, char* value, int64_t id)
{
    if (id == 0) {
        return 1;
    }

    char stmt[1024];
	snprintf(stmt, 1024,"INSERT INTO comments (comment, hash_id) VALUES ('%s', '%d');",	value, id);
	if (attempt_exec_nocallback(stmt, "Error adding comment: %s\n", hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite)) {
		return 1;
	}

    return 0;
}

/**
 * Add new name (e.g. a filename associated with a hash)
 *
 * @param hdb_info Hash database state info structure.
 * @return 1 on error and 0 on success
 */
uint8_t
sqlite_v1_addfilename(TSK_HDB_INFO * hdb_info, char* value, int64_t id)
{
    if (id == 0) {
        return 1;
    }

    char stmt[1024];
	snprintf(stmt, 1024, "INSERT INTO file_names (name, hash_id) VALUES ('%s', '%d');", value, id);
	if (attempt_exec_nocallback(stmt, "Error adding comment: %s\n", hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite)) {
		return 1;
	}

    return 0;
}

/**
 * This function is a no-op for SQLite hash database. The index is "internal" to the RDBMS.
 * @return 1 on error and 0 on success
 */
//uint8_t
//sqlite_v1_finalize(TSK_HDB_INFO * hdb_info)
//{
    // RJCTODO: Remove this after add to hash database function is made
    //if (end_transaction(hdb_info->idx_info)) {
	//	tsk_error_reset();
	//	tsk_error_set_errno(TSK_ERR_AUTO_DB);
	//	tsk_error_set_errstr("Failed to commit transaction\n");
 //       tsk_error_print(stderr);
	//	return 1;
	//}
	//
 //   // We create the indexes at the end in order to make adding the initial batch of data (e.g. indexing an NSRL db)
 //   // faster. Updates after indexing can be slower since the index has to update as well.
 //   if (need_SQL_index) {
 //       need_SQL_index = false;
	//    return attempt_exec_nocallback
	//	    ("CREATE INDEX md5_index ON hashes(md5);",
	//	    "Error creating md5_index on md5: %s\n", hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite) ||
	//	    attempt_exec_nocallback
	//	    ("CREATE INDEX sha1_index ON hashes(sha1);",
	//	    "Error creating sha1_index on sha1: %s\n", hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite);
 //   } else {
 //       return 0;
 //   }
//    return 0;
//}

/**
 * \ingroup hashdblib
 * Search the index for a text/ASCII hash value
 *
 * @param hdb_info Open hash database (with index)
 * @param hash Hash value to search for (NULL terminated string)
 * @param flags Flags to use in lookup
 * @param action Callback function to call for each hash db entry 
 * (not called if QUICK flag is given)
 * @param ptr Pointer to data to pass to each callback
 *
 * @return -1 on error, 0 if hash value not found, and 1 if value was found.
 */
int8_t
sqlite_v1_lookup_str(TSK_HDB_INFO * hdb_info, const char* hvalue,
                   TSK_HDB_FLAG_ENUM flags, TSK_HDB_LOOKUP_FN action,
                   void *ptr)
{
    int8_t ret = 0;
    hdb_info->idx_info->idx_struct.idx_sqlite_v1->lastId = 0;

#ifdef IDX_SQLITE_STORE_TEXT
    ret = lookup_text(hdb_info, hvalue, flags, action, ptr);
#else
	const size_t len = strlen(hvalue)/2;
	uint8_t * hashBlob = (uint8_t *) tsk_malloc(len+1);
	const char * pos = hvalue;
	size_t count = 0;

	for(count = 0; count < len; count++) {
		sscanf(pos, "%2hx", (short unsigned int *) &(hashBlob[count]));
		pos += 2 * sizeof(char);
	}

    ret = sqlite_v1_lookup_raw(hdb_info, hashBlob, len, flags, action, ptr);
#endif

    if ((ret == 1) && (hdb_info->db_type == TSK_HDB_DBTYPE_IDXONLY_ID)
        && !(flags & TSK_HDB_FLAG_QUICK) && (action != NULL)) {
        //name is blank because we don't have a name in this case
        ///@todo query the file_names table for associations
        char * name = "";
        action(hdb_info, hvalue, name, ptr);
    }

	return ret;		
}

/**
 * \ingroup hashdblib
 * Search the index for the given hash value given (in binary form).
 *
 * @param hdb_info Open hash database (with index)
 * @param hash Array with binary hash value to search for
 * @param len Number of bytes in binary hash value
 * @param flags Flags to use in lookup
 * @param action Callback function to call for each hash db entry 
 * (not called if QUICK flag is given)
 * @param ptr Pointer to data to pass to each callback
 *
 * @return -1 on error, 0 if hash value not found, and 1 if value was found.
 */
int8_t
sqlite_v1_lookup_raw(TSK_HDB_INFO * hdb_info, uint8_t * hvalue, uint8_t len,
                   TSK_HDB_FLAG_ENUM flags,
                   TSK_HDB_LOOKUP_FN action, void *ptr)
{
	char hashbuf[TSK_HDB_HTYPE_SHA2_256_LEN + 1];
	int8_t ret = 0;
    int i;
	TSK_OFF_T offset;
    char * selectStmt;
    sqlite3_stmt* stmt = NULL;

    tsk_take_lock(&hdb_info->lock);

	/* Sanity check */
	if ((hdb_info->hash_len)/2 != len) {
		tsk_error_reset();
		tsk_error_set_errno(TSK_ERR_HDB_ARG);
		tsk_error_set_errstr("hdb_lookup: Hash passed is different size than expected: %d vs %d",
			hdb_info->hash_len, (len * 2));
		ret = -1;
	} else {

    	if (hdb_info->hash_type == TSK_HDB_HTYPE_MD5_ID) {
            selectStmt = "SELECT md5,database_offset,id from hashes where md5=? limit 1";
        } else if (hdb_info->hash_type == TSK_HDB_HTYPE_SHA1_ID) {
            selectStmt = "SELECT sha1,database_offset,id from hashes where sha1=? limit 1";
        } else {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_HDB_ARG);
            tsk_error_set_errstr("Unknown hash type: %d\n", hdb_info->hash_type);
            ret = -1;
        }

        if (ret != -1) {
            prepare_stmt(selectStmt, &stmt, hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite);
        
	        if (attempt(sqlite3_bind_blob(stmt, 1, hvalue, len, free),
		        SQLITE_OK,
		        "Error binding binary blob: %s\n",
		        hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite)) {
			    ret = -1;
	        } else {
                // Found a match
	            if (sqlite3_step(stmt) == SQLITE_ROW) {
                    // save id
                    hdb_info->idx_info->idx_struct.idx_sqlite_v1->lastId = sqlite3_column_int64(stmt, 2);
                    
                    if ((flags & TSK_HDB_FLAG_QUICK)
			            || (hdb_info->db_type == TSK_HDB_DBTYPE_IDXONLY_ID)) {
				        
                        // There is just an index, so no other info to get
                        ///@todo Look up a name in the sqlite db
                        ret = 1;
		            } else {
                        // Use offset to get more info
			            for (i = 0; i < len; i++) {
				            hashbuf[2 * i] = hex[(hvalue[i] >> 4) & 0xf];
				            hashbuf[2 * i + 1] = hex[hvalue[i] & 0xf];
			            }
			            hashbuf[2 * len] = '\0';

			            offset = sqlite3_column_int64(stmt, 1);

			            if (hdb_info->getentry(hdb_info, hashbuf, offset, flags, action, ptr)) {
				            tsk_error_set_errstr2("hdb_lookup");
				            ret = -1;
			            } else {
			                ret = 1;
                        }
		            }
                }
            }
        
	        sqlite3_reset(stmt);
    
            if (stmt) {
                finalize_stmt(stmt);
            }
        }
    }

    tsk_release_lock(&hdb_info->lock);

	return ret;
}

/**
 * \ingroup hashdblib
 * Search the index for the given hash value given (in string form).
 *
 * @param hdb_info Open hash database (with index)
 * @param hash String hash value to search for
 * @param flags Flags to use in lookup
 * @param action Callback function to call for each hash db entry 
 * (not called if QUICK flag is given)
 * @param ptr Pointer to data to pass to each callback
 *
 * @return -1 on error, 0 if hash value not found, and 1 if value was found.
 */
///@todo refactor so as not to duplicate code with sqlite_v1_lookup_raw()
int8_t
lookup_text(TSK_HDB_INFO * hdb_info, const char* hvalue, TSK_HDB_FLAG_ENUM flags,
                   TSK_HDB_LOOKUP_FN action, void *ptr)
{
	int8_t ret = 0;
	TSK_OFF_T offset;
    char selectStmt[1024];
    sqlite3_stmt* stmt = NULL;
    int len = strlen(hvalue);

    tsk_take_lock(&hdb_info->lock);

	/* Sanity check */
	if (hdb_info->hash_len != len) {
		tsk_error_reset();
		tsk_error_set_errno(TSK_ERR_HDB_ARG);
		tsk_error_set_errstr("hdb_lookup: Hash passed is different size than expected: %d vs %d",
			hdb_info->hash_len, len);
		ret = -1;
	} else {
    	if (hdb_info->hash_type == TSK_HDB_HTYPE_MD5_ID) {
            snprintf(selectStmt, 1024,
		        "SELECT md5,database_offset,id from hashes where md5='%s' limit 1",
                hvalue);
        } else if (hdb_info->hash_type == TSK_HDB_HTYPE_SHA1_ID) {
            snprintf(selectStmt, 1024,
		        "SELECT sha1,database_offset,id from hashes where sha1='%s' limit 1",
                hvalue);
        } else {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_HDB_ARG);
            tsk_error_set_errstr("Unknown hash type: %d\n", hdb_info->hash_type);
            ret = -1;
        }

        if (ret != -1) {
            prepare_stmt(selectStmt, &stmt, hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite);
        
            // Found a match
	        if (sqlite3_step(stmt) == SQLITE_ROW) {
                // save id
                hdb_info->idx_info->idx_struct.idx_sqlite_v1->lastId = sqlite3_column_int64(stmt, 2);

                if ((flags & TSK_HDB_FLAG_QUICK)
			        || (hdb_info->db_type == TSK_HDB_DBTYPE_IDXONLY_ID)) {
				        
                    // There is just an index, so no other info to get
                    ///@todo Look up a name in the sqlite db
                    ret = 1;
		        } else {
                    // Use offset to get more info
			        offset = sqlite3_column_int64(stmt, 1);

			        if (hdb_info->getentry(hdb_info, hvalue, offset, flags, action, ptr)) {
				        tsk_error_set_errstr2("hdb_lookup");
				        ret = -1;
			        } else {
			            ret = 1;
                    }
		        }
            }
                   
	        sqlite3_reset(stmt);
    
            if (stmt) {
                finalize_stmt(stmt);
            }
        }
    }

    tsk_release_lock(&hdb_info->lock);

	return ret;
}

int8_t
getStrings(TSK_HDB_INFO * hdb_info, const char* selectStmt, std::vector<std::string>& out)
{
	int8_t ret = 0;
    sqlite3_stmt* stmt = NULL;
    int len = strlen(selectStmt);

    tsk_take_lock(&hdb_info->lock);

    prepare_stmt(selectStmt, &stmt, hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite);
        
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char* value = (const char *)sqlite3_column_text(stmt, 0);
        if (value != NULL) {
            std::string s(value);
            out.push_back(s);
        }
	}

	sqlite3_reset(stmt);
    
    if (stmt) {
        finalize_stmt(stmt);
    }

    tsk_release_lock(&hdb_info->lock);

	return ret;
}

/**
 * Convert binary blob hash string to text hash string
 * Returns the input if compiled in text hash mode.
 */
std::string blobToText(std::string binblob)
{
#ifdef IDX_SQLITE_STORE_TEXT
    return binblob; //already text
#else
    unsigned int blobsize = binblob.size();
    if (blobsize <= TSK_HDB_MAX_BINHASH_LEN) {
        char hashbuf[TSK_HDB_HTYPE_SHA2_256_LEN + 1];

		for (unsigned int i = 0; i < blobsize; i++) {
			hashbuf[2 * i] = hex[(binblob[i] >> 4) & 0xf];
			hashbuf[2 * i + 1] = hex[binblob[i] & 0xf];
		}
		hashbuf[2 * blobsize] = '\0';
        return std::string(&hashbuf[0]);
    } else {
        return "";
    }
#endif
}

/**
 * \ingroup hashdblib
 * Search the index for the given hash value given (in string form).
 *
 * @param hdb_info Open hash database (with index)
 * @param hashId   unique id of hash (corresponds to hashes.id)
 *
 * @return -1 on error, 0 if hash value not found, and 1 if value was found.
 */
void * sqlite_v1_getAllData(TSK_HDB_INFO * hdb_info, unsigned long hashId)
{
    SQliteHashStruct * h = new SQliteHashStruct();
    {
        std::vector<std::string> temp;
        char selectStmt[1024];
        snprintf(selectStmt, 1024, "SELECT md5 from hashes where id=%d", hashId);
        getStrings(hdb_info, selectStmt,  temp);
        if (temp.size() > 0) {
            h->hashMd5 = blobToText(temp.at(0));
        }
    }
    {
        std::vector<std::string> temp;
        char selectStmt[1024];
        snprintf(selectStmt, 1024, "SELECT sha1 from hashes where id=%d", hashId);
        getStrings(hdb_info, selectStmt,  temp);
        if (temp.size() > 0) {
            h->hashSha1 = blobToText(temp.at(0));
        }
    }
    {
        std::vector<std::string> temp;
        char selectStmt[1024];
        snprintf(selectStmt, 1024, "SELECT sha2_256 from hashes where id=%d", hashId);
        getStrings(hdb_info, selectStmt,  temp);
        if (temp.size() > 0) {
            h->hashSha2_256 = blobToText(temp.at(0));
        }
    }
    {
        char selectStmt[1024];
        snprintf(selectStmt, 1024, "SELECT name from file_names where hash_id=%d", hashId);
        getStrings(hdb_info, selectStmt,  h->names);
    }
    {
        char selectStmt[1024];
        snprintf(selectStmt, 1024, "SELECT comment from comments where hash_id=%d", hashId);
        getStrings(hdb_info, selectStmt,  h->comments);
    }
    return h;
}


/**
 * \ingroup hashdblib
 * Sets the updateable flag in the hdb_info argument based on querying the index props table.
 *
 * @param hdb_info Open hash database (with index)
 * @return -1 on error, 0 on success.
 */
//int8_t
//sqlite_v1_get_properties(TSK_HDB_INFO * hdb_info)
//{
//    int8_t ret = 0;
//	sqlite3_stmt* stmt = NULL;
//    char selectStmt[1024];
//
//    tsk_take_lock(&hdb_info->lock);
//    
//    snprintf(selectStmt, 1024, "SELECT value from db_properties where name='%s'", IDX_HASHSET_UPDATEABLE);
//    prepare_stmt(selectStmt, &stmt, hdb_info->idx_info->idx_struct.idx_sqlite_v1->hIdx_sqlite);
//
//	if (sqlite3_step(stmt) == SQLITE_ROW) {
//		const char* value = (const char *)sqlite3_column_text(stmt, 0);
//
//        if (value == NULL) {
//            tsk_error_set_errstr2("sqlite_v1_get_properties: null value");
//            ret = -1;
//        } else {
//            // Set the updateable flag
//            if (strcmp(value, "true") == 0) {
//                hdb_info->idx_info->updateable = 1;
//            }
//        }
//	} else {
//        tsk_error_set_errstr2("sqlite_v1_get_properties");
//        ret = -1;
//    }
//
//	sqlite3_reset(stmt);
//    
//    if (stmt) {
//        finalize_stmt(stmt);
//    }
//
//
//    ///@todo load db name property as well?
//
//    tsk_release_lock(&hdb_info->lock);
//
//	return ret;
//}

/*
 * Close the sqlite index handle
 * @param idx_info the index to close
 */
// RJCTODO: Need a close function at the TSK_HDB_INFO level
void
sqlite_hdb_close(TSK_HDB_INFO* hdb_info)
{
    TSK_SQLITE_HDB_INFO *db_info = (TSK_SQLITE_HDB_INFO*)hdb_info; 

    if (m_stmt) {
        finalize_stmt(m_stmt);
    }

    m_stmt = NULL;

    if (db_info->db) {
        sqlite3_close(db_info->db);
    }

    // RJCTODO: Cleanup the base stuff...
}

/**
 * Test the file to see if it is an sqlite database (== index only)
 *
 * @param hFile File handle to hash database
 *
 * @return 1 if sqlite and 0 if not
 */
uint8_t
sqlite3_test(FILE * hFile)
{
    const int header_size = 16;
    char header[header_size];

    if (hFile) {
        if (1 != fread(header, header_size, 1, hFile)) {
            ///@todo should this actually be an error?
            return 0;
        }
        else if (strncmp(header,
                IDX_SQLITE_V1_HEADER,
                strlen(IDX_SQLITE_V1_HEADER)) == 0) {
            return 1;
        }
    }

    return 0;
}

// RJCTODO: Comment
TSK_HDB_INFO *sqlite_hdb_open(TSK_TCHAR *db_path)
{
    TSK_SQLITE_HDB_INFO *sqlite_hdb_info = NULL;
    size_t flen = 0;

    assert(NULL != db_path);

    if ((sqlite_hdb_info = (TSK_SQLITE_HDB_INFO*)tsk_malloc(sizeof(TSK_SQLITE_HDB_INFO))) == NULL) {
        return NULL;
    }

    flen = TSTRLEN(db_path) + 8; // RJCTODO: Check this change from 32 (change was in DF code) with Brian; was change in older code? What is the point, anyway?
    sqlite_hdb_info->base.db_fname = (TSK_TCHAR*)tsk_malloc(flen * sizeof(TSK_TCHAR));
    if (NULL == sqlite_hdb_info->base.db_fname) {
        return NULL;
    }

    TSTRNCPY(sqlite_hdb_info->base.db_fname, db_path, flen);
    sqlite_hdb_info->base.db_type = TSK_HDB_DBTYPE_SQLITE_ID;
    sqlite_hdb_info->base.updateable = 1;
    sqlite_hdb_info->base.uses_external_index = 0;
    sqlite_hdb_info->base.hash_type = TSK_HDB_HTYPE_INVALID_ID; // This will be set when the index is created/opened. 
    sqlite_hdb_info->base.hash_len = 0; // This will be set when the index is created/opened.
    tsk_init_lock(&sqlite_hdb_info->base.lock);
    sqlite_hdb_info->base.makeindex = sqlite_hdb_make_index;
    sqlite_hdb_info->base.add_comment = sqlite_v1_addcomment; // RJCTODO: Consider moving this
    sqlite_hdb_info->base.add_filename = sqlite_v1_addfilename; // RJCTODO: Consider moving this

    sqlite_hdb_info->db = sqlite_hdb_open_db(db_path);
    if (NULL == sqlite_hdb_info->db) {
        free(sqlite_hdb_info->base.db_fname);
        free(sqlite_hdb_info);
        return NULL;
    }

    sqlite_hdb_info->last_id = 0;

    return (TSK_HDB_INFO*)sqlite_hdb_info;
}