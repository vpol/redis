/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum,
                              robj *dstkey, int op);

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a regular
 * hash table. */
robj *setTypeCreate(robj *value) {
    if (isObjectRepresentableAsLongLong(value, NULL) == REDIS_OK)
        return createIntsetObject();
    return createSetObject();
}

/* Add the specified value into a set. The function takes care of incrementing
 * the reference count of the object if needed in order to retain a copy.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictAdd(subject->ptr, value, NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr, llval, &success);
            if (success) {
                /* Convert to regular set when the intset contains
                 * too many entries. */
                if (intsetLen(subject->ptr) > server.set_max_intset_entries)
                    setTypeConvert(subject, REDIS_ENCODING_HT);
                return 1;
            }
        } else {
            /* Failed to get integer from object, convert to regular set. */
            setTypeConvert(subject, REDIS_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. */
            redisAssertWithInfo(NULL, value,
                                dictAdd(subject->ptr, value, NULL) == DICT_OK);
            incrRefCount(value);
            return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeRemove(robj *setobj, robj *value) {
    long long llval;
    if (setobj->encoding == REDIS_ENCODING_HT) {
        if (dictDelete(setobj->ptr, value) == DICT_OK) {
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr, llval, &success);
            if (success) return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeIsMember(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictFind((dict *) subject->ptr, value) != NULL;
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value, &llval) == REDIS_OK) {
            return intsetFind((intset *) subject->ptr, llval);
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

setTypeIterator *setTypeInitIterator(robj *subject) {
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        redisPanic("Unknown set encoding");
    }
    return si;
}

void setTypeReleaseIterator(setTypeIterator *si) {
    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position.
 *
 * Since set elements can be internally be stored as redis objects or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointer
 * (objele) or (llele) accordingly.
 *
 * Note that both the objele and llele pointers should be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no longer elements -1 is returned.
 * Returned objects ref count is not incremented, so this function is
 * copy on write friendly. */
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele) {
    if (si->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *objele = dictGetKey(de);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr, si->ii++, llele))
            return -1;
        *objele = NULL; /* Not needed. Defensive. */
    } else {
        redisPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new objects
 * or incrementing the ref count of returned objects. So if you don't
 * retain a pointer to this object you should call decrRefCount() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue as the result will be anyway of incrementing the ref count. */
robj *setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    robj *objele;
    int encoding;

    encoding = setTypeNext(si, &objele, &intele);
    switch (encoding) {
        case -1:
            return NULL;
        case REDIS_ENCODING_INTSET:
            return createStringObjectFromLongLong(intele);
        case REDIS_ENCODING_HT:
            incrRefCount(objele);
            return objele;
        default:
            redisPanic("Unsupported encoding");
    }
    return NULL; /* just to suppress warnings */
}

/* Return random element from a non empty set.
 * The returned element can be a int64_t value if the set is encoded
 * as an "intset" blob of integers, or a redis object if the set
 * is a regular set.
 *
 * The caller provides both pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and is used by the caller to check if the
 * int64_t pointer or the redis object pointer was populated.
 *
 * Note that both the objele and llele pointers should be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When an object is returned (the set was a real set) the ref count
 * of the object is not incremented so this function can be considered
 * copy on write friendly. */
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele) {
    if (setobj->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictGetRandomKey(setobj->ptr);
        *objele = dictGetKey(de);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
        *objele = NULL; /* Not needed. Defensive. */
    } else {
        redisPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

unsigned long setTypeSize(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictSize((dict *) subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset *) subject->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeIterator *si;
    redisAssertWithInfo(NULL, setobj, setobj->type == REDIS_SET &&
                                      setobj->encoding == REDIS_ENCODING_INTSET);

    if (enc == REDIS_ENCODING_HT) {
        int64_t intele;
        dict *d = dictCreate(&setDictType, NULL);
        robj *element;

        /* Presize the dict to avoid rehashing */
        dictExpand(d, intsetLen(setobj->ptr));

        /* To add the elements we extract integers and create redis objects */
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, &element, &intele) != -1) {
            element = createStringObjectFromLongLong(intele);
            redisAssertWithInfo(NULL, element,
                                dictAdd(d, element, NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        setobj->encoding = REDIS_ENCODING_HT;
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        redisPanic("Unsupported set conversion");
    }
}

void saddCommand(redisClient *c) {
    robj *set;
    int j, added = 0;

    set = lookupKeyWrite(c->db, c->argv[1]);
    if (set == NULL) {
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db, c->argv[1], set);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c, shared.wrongtypeerr);
            return;
        }
    }

    for (j = 2; j < c->argc; j++) {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (setTypeAdd(set, c->argv[j])) added++;
    }
    if (added) {
        signalModifiedKey(c->db, c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_SET, "sadd", c->argv[1], c->db->id);
    }
    server.dirty += added;
    addReplyLongLong(c, added);
}

void sremCommand(redisClient *c) {
    robj *set;
    int j, deleted = 0, keyremoved = 0;

    if ((set = lookupKeyWriteOrReply(c, c->argv[1], shared.czero)) == NULL ||
        checkType(c, set, REDIS_SET))
        return;

    for (j = 2; j < c->argc; j++) {
        if (setTypeRemove(set, c->argv[j])) {
            deleted++;
            if (setTypeSize(set) == 0) {
                dbDelete(c->db, c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
    if (deleted) {
        signalModifiedKey(c->db, c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_SET, "srem", c->argv[1], c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

void smoveCommand(redisClient *c) {
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->db, c->argv[1]);
    dstset = lookupKeyWrite(c->db, c->argv[2]);
    ele = c->argv[3] = tryObjectEncoding(c->argv[3]);

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {
        addReply(c, shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c, srcset, REDIS_SET) ||
        (dstset && checkType(c, dstset, REDIS_SET)))
        return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
        addReply(c, setTypeIsMember(srcset, ele) ? shared.cone : shared.czero);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset, ele)) {
        addReply(c, shared.czero);
        return;
    }
    notifyKeyspaceEvent(REDIS_NOTIFY_SET, "srem", c->argv[1], c->db->id);

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) {
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
    signalModifiedKey(c->db, c->argv[1]);
    signalModifiedKey(c->db, c->argv[2]);
    server.dirty++;

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate(ele);
        dbAdd(c->db, c->argv[2], dstset);
    }

    /* An extra key has changed when ele was successfully added to dstset */
    if (setTypeAdd(dstset, ele)) {
        server.dirty++;
        notifyKeyspaceEvent(REDIS_NOTIFY_SET, "sadd", c->argv[2], c->db->id);
    }
    addReply(c, shared.cone);
}

void sismemberCommand(redisClient *c) {
    robj *set;

    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL ||
        checkType(c, set, REDIS_SET))
        return;

    c->argv[2] = tryObjectEncoding(c->argv[2]);
    if (setTypeIsMember(set, c->argv[2]))
        addReply(c, shared.cone);
    else
        addReply(c, shared.czero);
}

void scardCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL ||
        checkType(c, o, REDIS_SET))
        return;

    addReplyLongLong(c, setTypeSize(o));
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. */
#define SPOP_MOVE_STRATEGY_MUL 5

void spopWithCountCommand(redisClient *c) {
    long l;
    unsigned long count, size;
    robj *set;

    /* Get the count argument */
    if (getLongFromObjectOrReply(c, c->argv[2], &l, NULL) != REDIS_OK) return;
    if (l >= 0) {
        count = (unsigned) l;
    } else {
        addReply(c, shared.outofrangeerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set. Otherwise, return nil */
    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.emptymultibulk))
        == NULL || checkType(c, set, REDIS_SET))
        return;

    /* If count is zero, serve an empty multibulk ASAP to avoid special
     * cases later. */
    if (count == 0) {
        addReply(c, shared.emptymultibulk);
        return;
    }

    size = setTypeSize(set);

    /* Generate an SPOP keyspace notification */
    notifyKeyspaceEvent(REDIS_NOTIFY_SET, "spop", c->argv[1], c->db->id);
    server.dirty += count;

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set. */
    if (count >= size) {
        /* We just return the entire set */
        sunionDiffGenericCommand(c, c->argv + 1, 1, NULL, REDIS_OP_UNION);

        /* Delete the set as it is now empty */
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[1], c->db->id);

        /* Propagate this command as an DEL operation */
        rewriteClientCommandVector(c, 2, shared.del, c->argv[1]);
        signalModifiedKey(c->db, c->argv[1]);
        server.dirty++;
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SERM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. */
    robj *propargv[3];
    propargv[0] = createStringObject("SREM", 4);
    propargv[1] = c->argv[1];
    addReplyMultiBulkLen(c, count);

    /* Common iteration vars. */
    robj *objele;
    int encoding;
    int64_t llele;
    unsigned long remaining = size - count; /* Elements left after SPOP. */

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set. Also we are sure that count < size.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. */
    if (remaining * SPOP_MOVE_STRATEGY_MUL > count) {
        while (count--) {
            encoding = setTypeRandomElement(set, &objele, &llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                objele = createStringObjectFromLongLong(llele);
            } else {
                incrRefCount(objele);
            }

            /* Return the element to the client and remove from the set. */
            addReplyBulk(c, objele);
            setTypeRemove(set, objele);

            /* Replicate/AOF this command as an SREM operation */
            propargv[2] = objele;
            alsoPropagate(server.sremCommand, c->db->id, propargv, 3,
                          REDIS_PROPAGATE_AOF | REDIS_PROPAGATE_REPL);
            decrRefCount(objele);
        }
    } else {
        /* CASE 3: The number of elements to return is very big, approaching
         * the size of the set itself. After some time extracting random elements
         * from such a set becomes computationally expensive, so we use
         * a different strategy, we extract random elements that we don't
         * want to return (the elements that will remain part of the set),
         * creating a new set as we do this (that will be stored as the original
         * set). Then we return the elements left in the original set and
         * release it. */
        robj *newset = NULL;

        /* Create a new set with just the remaining elements. */
        while (remaining--) {
            encoding = setTypeRandomElement(set, &objele, &llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                objele = createStringObjectFromLongLong(llele);
            } else {
                incrRefCount(objele);
            }
            if (!newset) newset = setTypeCreate(objele);
            setTypeAdd(newset, objele);
            setTypeRemove(set, objele);
            decrRefCount(objele);
        }

        /* Assign the new set as the key value. */
        incrRefCount(set); /* Protect the old set value. */
        dbOverwrite(c->db, c->argv[1], newset);

        /* Tranfer the old set to the client and release it. */
        setTypeIterator *si;
        si = setTypeInitIterator(set);
        while ((encoding = setTypeNext(si, &objele, &llele)) != -1) {
            if (encoding == REDIS_ENCODING_INTSET) {
                objele = createStringObjectFromLongLong(llele);
            } else {
                incrRefCount(objele);
            }
            addReplyBulk(c, objele);

            /* Replicate/AOF this command as an SREM operation */
            propargv[2] = objele;
            alsoPropagate(server.sremCommand, c->db->id, propargv, 3,
                          REDIS_PROPAGATE_AOF | REDIS_PROPAGATE_REPL);

            decrRefCount(objele);
        }
        setTypeReleaseIterator(si);
        decrRefCount(set);
    }

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. */
    decrRefCount(propargv[0]);
    preventCommandPropagation(c);
}

void spopCommand(redisClient *c) {
    robj *set, *ele, *aux;
    int64_t llele;
    int encoding;

    if (c->argc == 3) {
        spopWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReply(c, shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set */
    if ((set = lookupKeyWriteOrReply(c, c->argv[1], shared.nullbulk)) == NULL ||
        checkType(c, set, REDIS_SET))
        return;

    /* Get a random element from the set */
    encoding = setTypeRandomElement(set, &ele, &llele);

    /* Remove the element from the set */
    if (encoding == REDIS_ENCODING_INTSET) {
        ele = createStringObjectFromLongLong(llele);
        set->ptr = intsetRemove(set->ptr, llele, NULL);
    } else {
        incrRefCount(ele);
        setTypeRemove(set, ele);
    }

    notifyKeyspaceEvent(REDIS_NOTIFY_SET, "spop", c->argv[1], c->db->id);

    /* Replicate/AOF this command as an SREM operation */
    aux = createStringObject("SREM", 4);
    rewriteClientCommandVector(c, 3, aux, c->argv[1], ele);
    decrRefCount(ele);
    decrRefCount(aux);

    /* Add the element to the reply */
    addReplyBulk(c, ele);

    /* Delete the set if it's empty */
    if (setTypeSize(set) == 0) {
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }

    /* Set has been modified */
    signalModifiedKey(c->db, c->argv[1]);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

void srandmemberWithCountCommand(redisClient *c) {
    long l;
    unsigned long count, size;
    int uniq = 1;
    robj *set, *ele;
    int64_t llele;
    int encoding;

    dict *d;

    if (getLongFromObjectOrReply(c, c->argv[3], &l, NULL) != REDIS_OK) return;
    if (l >= 0) {
        count = (unsigned) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.emptymultibulk))
        == NULL || checkType(c, set, REDIS_SET))
        return;
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c, shared.emptymultibulk);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. */
    if (!uniq) {
        addReplyMultiBulkLen(c, count);
        while (count--) {
            encoding = setTypeRandomElement(set, &ele, &llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulk(c, ele);
            }
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {
        sunionDiffGenericCommand(c, c->argv + 1, 1, NULL, REDIS_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&setDictType, NULL);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requsted elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient. */
    if (count * SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        si = setTypeInitIterator(set);
        while ((encoding = setTypeNext(si, &ele, &llele)) != -1) {
            int retval = DICT_ERR;

            if (encoding == REDIS_ENCODING_INTSET) {
                retval = dictAdd(d, createStringObjectFromLongLong(llele), NULL);
            } else {
                retval = dictAdd(d, dupStringObject(ele), NULL);
            }
            redisAssert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        redisAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            dictEntry *de;

            de = dictGetRandomKey(d);
            dictDelete(d, dictGetKey(de));
            size--;
        }
    }

        /* CASE 4: We have a big set compared to the requested number of elements.
         * In this case we can simply get random elements from the set and add
         * to the temporary set, trying to eventually get enough unique elements
         * to reach the specified count. */
    else {
        unsigned long added = 0;

        while (added < count) {
            encoding = setTypeRandomElement(set, &ele, &llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                ele = createStringObjectFromLongLong(llele);
            } else {
                ele = dupStringObject(ele);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d, ele, NULL) == DICT_OK)
                added++;
            else
                decrRefCount(ele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        dictIterator *di;
        dictEntry *de;

        addReplyMultiBulkLen(c, count);
        di = dictGetIterator(d);
        while ((de = dictNext(di)) != NULL)
            addReplyBulk(c, dictGetKey(de));
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

void srandmemberCommand(redisClient *c) {
    robj *set, *ele;
    int64_t llele;
    int encoding;

    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReply(c, shared.syntaxerr);
        return;
    }

    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL ||
        checkType(c, set, REDIS_SET))
        return;

    encoding = setTypeRandomElement(set, &ele, &llele);
    if (encoding == REDIS_ENCODING_INTSET) {
        addReplyBulkLongLong(c, llele);
    } else {
        addReplyBulk(c, ele);
    }
}

void srandmemberstoreCommand(redisClient *c) {
    robj *dstkey, *set;
    robj *ele, *dstset = NULL;
    int64_t llele;
    long l;
    int cardinality = 0;
    unsigned long count, size;
    int uniq = 1;
    int encoding;
    dict *d;

    if (c->argc != 4) {
        addReply(c, shared.syntaxerr);
        return;
    }

    if ((set = lookupKeyReadOrReply(c, c->argv[2], shared.nullbulk)) == NULL ||
        checkType(c, set, REDIS_SET))
        return;

    if ((set = lookupKeyReadOrReply(c, c->argv[2], shared.emptymultibulk)) == NULL ||
        checkType(c, set, REDIS_SET))
        return;

    if (getLongFromObjectOrReply(c, c->argv[3], &l, NULL) != REDIS_OK) return;

    if (l >= 0) {
        count = (unsigned) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c, shared.syntaxerr);
        return;
    }

    dstkey = c->argv[1];
    dstset = createIntsetObject();

    /* CASE 1: The count was negative, so the extraction method is just:
     * "write to destination N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. */
    if (!uniq) {
        while (count--) {
            encoding = setTypeRandomElement(set, &ele, &llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                if (setTypeAdd(dstset, (robj *)llele)) cardinality++;
                decrRefCount((robj *)llele);
            } else {
                if (setTypeAdd(dstset, ele)) cardinality++;
                decrRefCount(ele);
            }
        }
    }


    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {
        sunionDiffGenericCommand(c, c->argv + 2, 1, dstkey, REDIS_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&setDictType, NULL);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requsted elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient. */
    if (count * SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        si = setTypeInitIterator(set);
        while ((encoding = setTypeNext(si, &ele, &llele)) != -1) {
            int retval = DICT_ERR;

            if (encoding == REDIS_ENCODING_INTSET) {
                retval = dictAdd(d, createStringObjectFromLongLong(llele), NULL);
            } else {
                retval = dictAdd(d, dupStringObject(ele), NULL);
            }
            redisAssert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        redisAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            dictEntry *de;
            de = dictGetRandomKey(d);
            dictDelete(d, dictGetKey(de));
            size--;
        }
    }

        /* CASE 4: We have a big set compared to the requested number of elements.
         * In this case we can simply get random elements from the set and add
         * to the temporary set, trying to eventually get enough unique elements
         * to reach the specified count. */
    else {
        unsigned long added = 0;

        while (added < count) {
            encoding = setTypeRandomElement(set, &ele, &llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                ele = createStringObjectFromLongLong(llele);
            } else {
                ele = dupStringObject(ele);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d, ele, NULL) == DICT_OK)
                added++;
            else
                decrRefCount(ele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        dictIterator *di;
        dictEntry *de;

//        addReplyMultiBulkLen(c, count);
        di = dictGetIterator(d);
        while ((de = dictNext(di)) != NULL)
            if (setTypeAdd(dstset, dictGetKey(de))) cardinality++;
//        decrRefCount(ele);
//            addReplyBulk(c, dictGetKey(de));
        dictReleaseIterator(di);
        dictRelease(d);
    }

    /* If we have a target key where to store the resulting set
     * create this key with the result set inside */
    int deleted = dbDelete(c->db, dstkey);
    if (setTypeSize(dstset) > 0) {
        dbAdd(c->db, dstkey, dstset);
        addReplyLongLong(c, setTypeSize(dstset));
        notifyKeyspaceEvent(REDIS_NOTIFY_SET,
                            "srandmemberstore",
                            dstkey,
                            c->db->id);
    } else {
        decrRefCount(dstset);
        addReply(c, shared.czero);
        if (deleted)
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del",
                                dstkey, c->db->id);
    }
    signalModifiedKey(c->db, dstkey);
    server.dirty++;
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    return setTypeSize(*(robj **) s1) - setTypeSize(*(robj **) s2);
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = *(robj **) s1, *o2 = *(robj **) s2;

    return (o2 ? setTypeSize(o2) : 0) - (o1 ? setTypeSize(o1) : 0);
}

void sinterGenericCommand(redisClient *c, robj **setkeys,
                          unsigned long setnum, robj *dstkey) {
    robj **sets = zmalloc(sizeof(robj *) * setnum);
    setTypeIterator *si;
    robj *eleobj, *dstset = NULL;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding;

    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
                       lookupKeyWrite(c->db, setkeys[j]) :
                       lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            zfree(sets);
            if (dstkey) {
                if (dbDelete(c->db, dstkey)) {
                    signalModifiedKey(c->db, dstkey);
                    server.dirty++;
                }
                addReply(c, shared.czero);
            } else {
                addReply(c, shared.emptymultibulk);
            }
            return;
        }
        if (checkType(c, setobj, REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(sets, setnum, sizeof(robj *), qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey) {
        replylen = addDeferredMultiBulkLength(c);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    si = setTypeInitIterator(sets[0]);
    while ((encoding = setTypeNext(si, &eleobj, &intobj)) != -1) {
        for (j = 1; j < setnum; j++) {
            if (sets[j] == sets[0]) continue;
            if (encoding == REDIS_ENCODING_INTSET) {
                /* intset with intset is simple... and fast */
                if (sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset *) sets[j]->ptr, intobj)) {
                    break;
                    /* in order to compare an integer with an object we
                     * have to use the generic function, creating an object
                     * for this */
                } else if (sets[j]->encoding == REDIS_ENCODING_HT) {
                    eleobj = createStringObjectFromLongLong(intobj);
                    if (!setTypeIsMember(sets[j], eleobj)) {
                        decrRefCount(eleobj);
                        break;
                    }
                    decrRefCount(eleobj);
                }
            } else if (encoding == REDIS_ENCODING_HT) {
                /* Optimization... if the source object is integer
                 * encoded AND the target set is an intset, we can get
                 * a much faster path. */
                if (eleobj->encoding == REDIS_ENCODING_INT &&
                    sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset *) sets[j]->ptr, (long) eleobj->ptr)) {
                    break;
                    /* else... object to object check is easy as we use the
                     * type agnostic API here. */
                } else if (!setTypeIsMember(sets[j], eleobj)) {
                    break;
                }
            }
        }

        /* Only take action when all sets contain the member */
        if (j == setnum) {
            if (!dstkey) {
                if (encoding == REDIS_ENCODING_HT)
                    addReplyBulk(c, eleobj);
                else
                    addReplyBulkLongLong(c, intobj);
                cardinality++;
            } else {
                if (encoding == REDIS_ENCODING_INTSET) {
                    eleobj = createStringObjectFromLongLong(intobj);
                    setTypeAdd(dstset, eleobj);
                    decrRefCount(eleobj);
                } else {
                    setTypeAdd(dstset, eleobj);
                }
            }
        }
    }
    setTypeReleaseIterator(si);

    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        int deleted = dbDelete(c->db, dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db, dstkey, dstset);
            addReplyLongLong(c, setTypeSize(dstset));
            notifyKeyspaceEvent(REDIS_NOTIFY_SET, "sinterstore",
                                dstkey, c->db->id);
        } else {
            decrRefCount(dstset);
            addReply(c, shared.czero);
            if (deleted)
                notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del",
                                    dstkey, c->db->id);
        }
        signalModifiedKey(c->db, dstkey);
        server.dirty++;
    } else {
        setDeferredMultiBulkLength(c, replylen, cardinality);
    }
    zfree(sets);
}

void sinterCommand(redisClient *c) {
    sinterGenericCommand(c, c->argv + 1, c->argc - 1, NULL);
}

void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1]);
}

#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2

void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum,
                              robj *dstkey, int op) {
    robj **sets = zmalloc(sizeof(robj *) * setnum);
    setTypeIterator *si;
    robj *ele, *dstset = NULL;
    int j, cardinality = 0;
    int diff_algo = 1;

    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
                       lookupKeyWrite(c->db, setkeys[j]) :
                       lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c, setobj, REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. */
    if (op == REDIS_OP_DIFF && sets[0]) {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            algo_one_work += setTypeSize(sets[0]);
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets + 1, setnum - 1, sizeof(robj *),
                  qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createIntsetObject();

    if (op == REDIS_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while ((ele = setTypeNextObject(si)) != NULL) {
                if (setTypeAdd(dstset, ele)) cardinality++;
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si);
        }
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. */
        si = setTypeInitIterator(sets[0]);
        while ((ele = setTypeNextObject(si)) != NULL) {
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; /* no key is an empty set. */
                if (sets[j] == sets[0]) break; /* same set! */
                if (setTypeIsMember(sets[j], ele)) break;
            }
            if (j == setnum) {
                /* There is no other set with this element. Add it. */
                setTypeAdd(dstset, ele);
                cardinality++;
            }
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 2) {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while ((ele = setTypeNextObject(si)) != NULL) {
                if (j == 0) {
                    if (setTypeAdd(dstset, ele)) cardinality++;
                } else {
                    if (setTypeRemove(dstset, ele)) cardinality--;
                }
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplyMultiBulkLen(c, cardinality);
        si = setTypeInitIterator(dstset);
        while ((ele = setTypeNextObject(si)) != NULL) {
            addReplyBulk(c, ele);
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);
        decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        int deleted = dbDelete(c->db, dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db, dstkey, dstset);
            addReplyLongLong(c, setTypeSize(dstset));
            notifyKeyspaceEvent(REDIS_NOTIFY_SET,
                                op == REDIS_OP_UNION ? "sunionstore" : "sdiffstore",
                                dstkey, c->db->id);
        } else {
            decrRefCount(dstset);
            addReply(c, shared.czero);
            if (deleted)
                notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del",
                                    dstkey, c->db->id);
        }
        signalModifiedKey(c->db, dstkey);
        server.dirty++;
    }
    zfree(sets);
}

void sunionCommand(redisClient *c) {
    sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, REDIS_OP_UNION);
}

void sunionstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1], REDIS_OP_UNION);
}

void sdiffCommand(redisClient *c) {
    sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, REDIS_OP_DIFF);
}

void sdiffstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1], REDIS_OP_DIFF);
}

void sscanCommand(redisClient *c) {
    robj *set;
    unsigned long cursor;

    if (parseScanCursorOrReply(c, c->argv[2], &cursor) == REDIS_ERR) return;
    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.emptyscan)) == NULL ||
        checkType(c, set, REDIS_SET))
        return;
    scanGenericCommand(c, set, cursor);
}
