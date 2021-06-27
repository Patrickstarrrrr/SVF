/// PTData (AbstractPointsToDS.h) implementations with a persistent backend.
/// Each Key is given a cheap points-to ID which refers to some real points-to set.

#ifndef PERSISTENT_POINTSTO_H_
#define PERSISTENT_POINTSTO_H_

#include "MemoryModel/AbstractPointsToDS.h"
#include "MemoryModel/PersistentPointsToCache.h"
#include "Util/SVFUtil.h"

namespace SVF
{

template <typename Key, typename KeySet, typename Data, typename DataSet>
class PersistentDFPTData;
template <typename Key, typename KeySet, typename Data, typename DataSet>
class PersistentIncDFPTData;
template <typename Key, typename KeySet, typename Data, typename DataSet, typename VersionedKey, typename VersionedKeySet>
class PersistentVersionedPTData;

/// PTData backed by a PersistentPointsToCache.
template <typename Key, typename KeySet, typename Data, typename DataSet>
class PersistentPTData : public PTData<Key, KeySet, Data, DataSet>
{
    template <typename K, typename KS, typename D, typename DS, typename VK, typename VKS>
    friend class PersistentVersionedPTData;
    friend class PersistentDFPTData<Key, KeySet, Data, DataSet>;
    friend class PersistentIncDFPTData<Key, KeySet, Data, DataSet>;
public:
    typedef PTData<Key, KeySet, Data, DataSet> BasePTData;
    typedef typename BasePTData::PTDataTy PTDataTy;

    typedef Map<Key, PointsToID> KeyToIDMap;
    typedef Map<Data, KeySet> RevPtsMap;

    /// Constructor
    PersistentPTData(PersistentPointsToCache<DataSet> &cache, bool reversePT = true, PTDataTy ty = PTDataTy::PersBase)
        : BasePTData(reversePT, ty), ptCache(cache) { }

    virtual ~PersistentPTData() { }

    virtual inline void clear() override
    {
        ptsMap.clear();
        revPtsMap.clear();
    }

    virtual inline const DataSet& getPts(const Key &var) override
    {
        PointsToID id = ptsMap[var];
        return ptCache.getActualPts(id);
    }

    virtual inline const KeySet& getRevPts(const Data &data) override
    {
        assert(this->rev && "PersistentPTData::getRevPts: constructed without reverse PT support!");
        return revPtsMap[data];
    }

    virtual inline bool addPts(const Key &dstKey, const Data &element) override
    {
        DataSet srcPts;
        srcPts.set(element);
        PointsToID srcId = ptCache.emplacePts(srcPts);
        return unionPtsFromId(dstKey, srcId);
    }

    virtual inline bool unionPts(const Key& dstKey, const Key& srcKey) override
    {
        PointsToID srcId = ptsMap[srcKey];
        return unionPtsFromId(dstKey, srcId);
    }

    virtual inline bool unionPts(const Key& dstKey, const DataSet& srcData) override
    {
        PointsToID srcId = ptCache.emplacePts(srcData);
        return unionPtsFromId(dstKey, srcId);
    }

    virtual inline void dumpPTData() override
    {
    }

    virtual void clearPts(const Key &var, const Data &element) override
    {
        DataSet toRemoveData;
        toRemoveData.set(element);
        PointsToID toRemoveId = ptCache.emplacePts(toRemoveData);
        PointsToID varId = ptsMap[var];
        PointsToID complementId = ptCache.complementPts(varId, toRemoveId);
        if (varId != complementId)
        {
            ptsMap[var] = complementId;
            clearSingleRevPts(revPtsMap[element], var);
        }
    }

    virtual void clearFullPts(const Key& var) override
    {
        clearRevPts(getPts(var), var);
        ptsMap[var] = PersistentPointsToCache<DataSet>::emptyPointsToId();
    }

    virtual std::pair<u64_t, u64_t> topN(const unsigned n) const override
    {
        // How many pointers does each points-to set have?
        Map<PointsToID, u64_t> ptCounts;
        u64_t keys = 0;
        for (const typename KeyToIDMap::value_type &pi : ptsMap)
        {
            if (pi.second != PersistentPointsToCache<DataSet>::emptyPointsToId())
            {
                ++ptCounts[pi.second];
                ++keys;
            }
        }

        // Which points-to sets are the n most common?
        std::vector<u64_t> counts;
        for (const Map<PointsToID, u64_t>::value_type &ic : ptCounts) counts.push_back(ic.second);
        std::sort(counts.begin(), counts.end(), std::greater<>());

        // Sum of the counts of most common n points-to sets.
        u64_t mostCommonCount = 0;
        for (size_t i = 0; i < n && i < counts.size(); ++i) mostCommonCount += counts[i];

        return std::make_pair(mostCommonCount, keys);
    }

    virtual u64_t inUsePointsToSets(void) override
    {
        std::vector<const KeyToIDMap *> maps = { &ptsMap };
        return inUsePointsToSets(maps);
    }

    /// Methods to support type inquiry through isa, cast, and dyn_cast:
    ///@{
    static inline bool classof(const PersistentPTData<Key, KeySet, Data, DataSet> *)
    {
        return true;
    }

    static inline bool classof(const PTData<Key, KeySet, Data, DataSet>* ptd)
    {
        return ptd->getPTDTY() == PTDataTy::PersBase;
    }
    ///@}

private:
    /// Internal unionPts since other methods follow the same pattern.
    /// Renamed because PointsToID and Key may be the same type...
    inline bool unionPtsFromId(const Key &dstKey, PointsToID srcId)
    {
        PointsToID dstId = ptsMap[dstKey];
        PointsToID newDstId = ptCache.unionPts(dstId, srcId);

        bool changed = newDstId != dstId;
        if (changed)
        {
            ptsMap[dstKey] = newDstId;

            // Reverse points-to only needs to be handled when dst's
            // points-to set has changed (i.e., do it the first time only).
            if (this->rev)
            {
                const DataSet &srcPts = ptCache.getActualPts(srcId);
                for (const Data &d : srcPts) insertKey<Key, KeySet>(dstKey, revPtsMap[d]);
            }
        }

        return changed;
    }

    inline void clearSingleRevPts(KeySet &revSet, const Key &k)
    {
        if (this->rev)
        {
            SVFUtil::removeKey(k, revSet);
        }
    }

    inline void clearRevPts(const DataSet &pts, const Key &k)
    {
        if (this->rev)
        {
            for (const Data &d : pts) clearSingleRevPts(revPtsMap[d], k);
        }
    }

    /// Number of unique points-to sets in use for the KeyToIDMaps.
    static inline u64_t inUsePointsToSets(const std::vector<const KeyToIDMap *> maps)
    {
        Set<PointsToID> pointsToSets;
        for (const KeyToIDMap *kim : maps)
        {
            for (const typename KeyToIDMap::value_type &ki : *kim)
            {
                pointsToSets.insert(ki.second);
            }
        }

        return pointsToSets.size();
    }

protected:
    PersistentPointsToCache<DataSet> &ptCache;
    KeyToIDMap ptsMap;
    RevPtsMap revPtsMap;
};

/// DiffPTData implemented with a persistent points-to backing.
template <typename Key, typename KeySet, typename Data, typename DataSet>
class PersistentDiffPTData : public DiffPTData<Key, KeySet, Data, DataSet>
{
public:
    typedef PTData<Key, KeySet, Data, DataSet> BasePTData;
    typedef DiffPTData<Key, KeySet, Data, DataSet> BaseDiffPTData;
    typedef PersistentPTData<Key, KeySet, Data, DataSet> BasePersPTData;
    typedef typename BasePTData::PTDataTy PTDataTy;

    typedef typename BasePersPTData::KeyToIDMap KeyToIDMap;
    typedef typename BasePersPTData::RevPtsMap RevPtsMap;

    /// Constructor
    PersistentDiffPTData(PersistentPointsToCache<DataSet> &cache, bool reversePT = true, PTDataTy ty = PTDataTy::PersDiff)
        : BaseDiffPTData(reversePT, ty), persPTData(cache, reversePT), ptCache(cache) { }

    virtual ~PersistentDiffPTData() { }

    virtual inline void clear() override
    {
        persPTData.clear();
        diffPtsMap.clear();
        propaPtsMap.clear();
    }

    virtual inline const DataSet &getPts(const Key& var) override
    {
        return persPTData.getPts(var);
    }

    virtual inline const KeySet& getRevPts(const Data &data) override
    {
        assert(this->rev && "PersistentDiffPTData::getRevPts: constructed without reverse PT support!");
        return persPTData.getRevPts(data);
    }

    virtual inline bool addPts(const Key &dstKey, const Data &element) override
    {
        return persPTData.addPts(dstKey, element);
    }

    virtual inline bool unionPts(const Key& dstKey, const Key& srcKey) override
    {
        return persPTData.unionPts(dstKey, srcKey);
    }

    virtual inline bool unionPts(const Key &dstKey, const DataSet &srcData) override
    {
        return persPTData.unionPts(dstKey, srcData);
    }

    virtual void clearPts(const Key &var, const Data &element) override
    {
        return persPTData.clearPts(var, element);
    }

    virtual void clearFullPts(const Key &var) override
    {
        return persPTData.clearFullPts(var);
    }

    virtual inline void dumpPTData() override
    {
        // TODO.
    }

    virtual inline const DataSet &getDiffPts(Key &var) override
    {
        PointsToID id = diffPtsMap[var];
        return ptCache.getActualPts(id);
    }

    virtual inline bool computeDiffPts(Key &var, const DataSet &all) override
    {
        PointsToID propaId = propaPtsMap[var];
        PointsToID allId = ptCache.emplacePts(all);
        // Diff is made up of the entire points-to set minus what has been propagated.
        PointsToID diffId = ptCache.complementPts(allId, propaId);
        diffPtsMap[var] = diffId;

        // We've now propagated the entire thing.
        propaPtsMap[var] = allId;

        // Whether diff is empty or not; just need to check against the ID since it
        // is the only empty set.
        return diffId != ptCache.emptyPointsToId();
    }

    virtual inline void updatePropaPtsMap(Key &src, Key &dst) override
    {
        PointsToID dstId = propaPtsMap[dst];
        PointsToID srcId = propaPtsMap[src];
        propaPtsMap[dst] = ptCache.intersectPts(dstId, srcId);
    }

    virtual inline void clearPropaPts(Key &var) override
    {
        propaPtsMap[var] = ptCache.emptyPointsToId();
    }

    std::pair<u64_t, u64_t> topN(const unsigned n) const override
    {
        return persPTData.topN(n);
    }

    virtual u64_t inUsePointsToSets(void) override
    {
        return persPTData.inUsePointsToSets();
    }

    /// Methods to support type inquiry through isa, cast, and dyn_cast:
    ///@{
    static inline bool classof(const PersistentDiffPTData<Key, KeySet, Data, DataSet> *)
    {
        return true;
    }

    static inline bool classof(const PTData<Key, KeySet, Data, DataSet>* ptd)
    {
        return ptd->getPTDTY() == PTDataTy::PersDiff;
    }
    ///@}

private:
    PersistentPointsToCache<DataSet> &ptCache;
    /// Backing to implement basic PTData methods. Allows us to avoid multiple inheritance.
    PersistentPTData<Key, KeySet, Data, DataSet> persPTData;
    /// Diff points-to to be propagated.
    KeyToIDMap diffPtsMap;
    /// Points-to already propagated.
    KeyToIDMap propaPtsMap;
};

/// DFPTData backed by a PersistentPointsToCache.
template <typename Key, typename KeySet, typename Data, typename DataSet>
class PersistentDFPTData : public DFPTData<Key, KeySet, Data, DataSet>
{
public:
    typedef PTData<Key, KeySet, Data, DataSet> BasePTData;
    typedef typename BasePTData::PTDataTy PTDataTy;
    typedef DFPTData<Key, KeySet, Data, DataSet> BaseDFPTData;
    typedef PersistentPTData<Key, KeySet, Data, DataSet> BasePersPTData;

    typedef typename BaseDFPTData::LocID LocID;
    typedef typename BasePersPTData::KeyToIDMap KeyToIDMap;
    typedef Map<LocID, KeyToIDMap> DFKeyToIDMap;

    PersistentDFPTData(PersistentPointsToCache<DataSet> &cache, bool reversePT = true, PTDataTy ty = PTDataTy::PersDataFlow)
        : BaseDFPTData(reversePT, ty), ptCache(cache), persPTData(cache, reversePT) { }

    virtual ~PersistentDFPTData() { }

    virtual inline void clear() override
    {
        // TODO: clear dfIn/OutPtsMaps.
        persPTData.clear();
    }

    virtual inline const DataSet &getPts(const Key& var) override
    {
        return persPTData.getPts(var);
    }

    virtual inline const KeySet& getRevPts(const Data &datum) override
    {
        assert(false && "PersistentDFPTData::getRevPts: not supported yet!");
    }

    virtual inline bool unionPts(const Key& dstKey, const Key& srcKey) override
    {
        return persPTData.unionPts(dstKey, srcKey);
    }

    virtual inline bool unionPts(const Key& dstKey, const DataSet &srcData) override
    {
        return persPTData.unionPts(dstKey, srcData);
    }

    virtual inline bool addPts(const Key &dstKey, const Data &element) override
    {
        return persPTData.addPts(dstKey, element);
    }

    virtual void clearPts(const Key& var, const Data &element) override
    {
        persPTData.clearPts(var, element);
    }

    virtual void clearFullPts(const Key& var) override
    {
        persPTData.clearFullPts(var);
    }

    virtual inline void dumpPTData() override
    {
        persPTData.dumpPTData();
    }

    virtual bool hasDFInSet(LocID loc) const override
    {
        return dfInPtsMap.find(loc) != dfInPtsMap.end();
    }

    virtual bool hasDFOutSet(LocID loc) const override
    {
        return dfOutPtsMap.find(loc) != dfOutPtsMap.end();
    }

    virtual bool hasDFInSet(LocID loc, const Key& var) const override
    {
        typename DFKeyToIDMap::const_iterator foundInKeyToId = dfInPtsMap.find(loc);
        if (foundInKeyToId == dfInPtsMap.end()) return false;
        const KeyToIDMap &inKeyToId = foundInKeyToId->second;
        return (inKeyToId.find(var) != inKeyToId.end());
    }

    virtual bool hasDFOutSet(LocID loc, const Key& var) const override
    {
        typename DFKeyToIDMap::const_iterator foundOutKeyToId = dfOutPtsMap.find(loc);
        if (foundOutKeyToId == dfOutPtsMap.end()) return false;
        const KeyToIDMap &outKeyToId = foundOutKeyToId->second;
        return (outKeyToId.find(var) != outKeyToId.end());
    }

    virtual const DataSet &getDFInPtsSet(LocID loc, const Key& var) override
    {
        PointsToID id = dfInPtsMap[loc][var];
        return ptCache.getActualPts(id);
    }

    virtual const DataSet &getDFOutPtsSet(LocID loc, const Key& var) override
    {
        PointsToID id = dfOutPtsMap[loc][var];
        return ptCache.getActualPts(id);
    }

    virtual bool updateDFInFromIn(LocID srcLoc, const Key &srcVar, LocID dstLoc, const Key &dstVar) override
    {
        return unionPtsThroughIds(getDFInPtIdRef(dstLoc, dstVar), getDFInPtIdRef(srcLoc, srcVar));
    }

    virtual bool updateAllDFInFromIn(LocID srcLoc, const Key &srcVar, LocID dstLoc, const Key &dstVar) override
    {
        return updateDFInFromIn(srcLoc, srcVar, dstLoc, dstVar);
    }

    virtual bool updateDFInFromOut(LocID srcLoc, const Key &srcVar, LocID dstLoc, const Key &dstVar) override
    {
        return unionPtsThroughIds(getDFInPtIdRef(dstLoc, dstVar), getDFOutPtIdRef(srcLoc, srcVar));
    }

    virtual bool updateAllDFInFromOut(LocID srcLoc, const Key &srcVar, LocID dstLoc, const Key &dstVar) override
    {
        return updateDFInFromOut(srcLoc, srcVar, dstLoc, dstVar);
    }

    virtual bool updateDFOutFromIn(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        return unionPtsThroughIds(getDFOutPtIdRef(dstLoc, dstVar), getDFInPtIdRef(srcLoc, srcVar));
    }

    virtual bool updateAllDFOutFromIn(LocID loc, const Key &singleton, bool strongUpdates) override
    {
        bool changed = false;
        if (this->hasDFInSet(loc))
        {
            const KeyToIDMap &inKeyToId = dfInPtsMap[loc];
            for (const typename KeyToIDMap::value_type &ki : inKeyToId)
            {
                const Key var = ki.first;
                /// Enable strong updates if required.
                if (strongUpdates && var == singleton) continue;

                if (updateDFOutFromIn(loc, var, loc, var)) changed = true;
            }
        }

        return changed;
    }

    virtual void clearAllDFOutUpdatedVar(LocID) override
    {
    }

    /// Update points-to set of top-level pointers with IN[srcLoc:srcVar].
    virtual bool updateTLVPts(LocID srcLoc, const Key &srcVar, const Key &dstVar) override
    {
        return unionPtsThroughIds(persPTData.ptsMap[dstVar], getDFInPtIdRef(srcLoc, srcVar));
    }

    virtual bool updateATVPts(const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        return unionPtsThroughIds(getDFOutPtIdRef(dstLoc, dstVar), persPTData.ptsMap[srcVar]);
    }

    std::pair<u64_t, u64_t> topN(const unsigned n) const override
    {
        // All points in this PTData mapping keys to PTSs.
        std::vector<const KeyToIDMap *> keyToIdMaps;
        keyToIdMaps.push_back(&persPTData.ptsMap);
        for (const DFKeyToIDMap *dfMap : { &dfInPtsMap, &dfOutPtsMap } )
        {
            for (const typename DFKeyToIDMap::value_type &lki : *dfMap)
            {
                keyToIdMaps.push_back(&lki.second);
            }
        }

        // How common are points-to sets across all maps and how many pointers are there?
        Map<PointsToID, u64_t> ptCounts;
        u64_t keys = 0;
        for (const KeyToIDMap *kim : keyToIdMaps)
        {
            for (const typename KeyToIDMap::value_type &ki : *kim)
            {
                // Ignore empty points-to sets.
                if (ki.second != PersistentPointsToCache<DataSet>::emptyPointsToId())
                {
                    ++ptCounts[ki.second];
                    ++keys;
                }
            }
        }

        // Which points-to sets are the n most common?
        std::vector<u64_t> counts;
        for (const Map<PointsToID, u64_t>::value_type &ic : ptCounts) counts.push_back(ic.second);
        std::sort(counts.begin(), counts.end(), std::greater<>());

        // Sum of the counts of most common n points-to sets.
        u64_t mostCommonCount = 0;
        for (size_t i = 0; i < n && i < counts.size(); ++i) mostCommonCount += counts[i];

        return std::make_pair(mostCommonCount, keys);
    }

    virtual u64_t inUsePointsToSets(void) override
    {
        std::vector<const KeyToIDMap *> maps;
        maps.push_back(&persPTData.ptsMap);

        for (const DFKeyToIDMap *dfMap : { &dfInPtsMap, &dfOutPtsMap } )
        {
            for (const typename DFKeyToIDMap::value_type &lkim : *dfMap)
            {
                maps.push_back(&lkim.second);
            }
        }

        return BasePersPTData::inUsePointsToSets(maps);
    }

    /// Methods to support type inquiry through isa, cast, and dyn_cast:
    ///@{
    static inline bool classof(const PersistentDFPTData<Key, KeySet, Data, DataSet> *)
    {
        return true;
    }

    static inline bool classof(const PTData<Key, KeySet, Data, DataSet> *ptd)
    {
        return ptd->getPTDTY() == PTDataTy::PersDataFlow
               || ptd->getPTDTY() == PTDataTy::PersIncDataFlow;
    }
    ///@}

protected:
    inline bool unionPtsThroughIds(PointsToID &dst, PointsToID &src)
    {
        PointsToID oldDst = dst;
        dst = ptCache.unionPts(dst, src);
        return oldDst != dst;
    }

    PointsToID &getDFInPtIdRef(LocID loc, const Key &var)
    {
        return dfInPtsMap[loc][var];
    }

    PointsToID &getDFOutPtIdRef(LocID loc, const Key &var)
    {
        return dfOutPtsMap[loc][var];
    }

protected:
    PersistentPointsToCache<DataSet> &ptCache;

    /// PTData for top-level pointers. We will also use its cache for address-taken pointers.
    PersistentPTData<Key, KeySet, Data, DataSet> persPTData;

    /// Address-taken points-to sets in IN-sets.
    DFKeyToIDMap dfInPtsMap;
    /// Address-taken points-to sets in OUT-sets.
    DFKeyToIDMap dfOutPtsMap;
};

/// Incremental version of the persistent data-flow points-to data structure.
template <typename Key, typename KeySet, typename Data, typename DataSet>
class PersistentIncDFPTData : public PersistentDFPTData<Key, KeySet, Data, DataSet>
{
public:
    typedef PTData<Key, KeySet, Data, DataSet> BasePTData;
    typedef PersistentPTData<Key, KeySet, Data, DataSet> BasePersPTData;
    typedef DFPTData<Key, KeySet, Data, DataSet> BaseDFPTData;
    typedef PersistentDFPTData<Key, KeySet, Data, DataSet> BasePersDFPTData;
    typedef typename BasePTData::PTDataTy PTDataTy;

    typedef typename BaseDFPTData::LocID LocID;
    typedef Map<LocID, KeySet> UpdatedVarMap;

public:
    /// Constructor
    PersistentIncDFPTData(PersistentPointsToCache<DataSet> &cache, bool reversePT = true, PTDataTy ty = BasePTData::PersIncDataFlow)
        : BasePersDFPTData(cache, reversePT, ty) { }

    virtual ~PersistentIncDFPTData() { }

    virtual inline bool updateDFInFromIn(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        if (varHasNewDFInPts(srcLoc, srcVar)
            && this->unionPtsThroughIds(this->getDFInPtIdRef(dstLoc, dstVar), this->getDFInPtIdRef(srcLoc, srcVar)))
        {
            setVarDFInSetUpdated(dstLoc, dstVar);
            return true;
        }

        return false;
    }

    virtual inline bool updateDFInFromOut(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        if (varHasNewDFOutPts(srcLoc, srcVar)
            && this->unionPtsThroughIds(this->getDFInPtIdRef(dstLoc, dstVar), this->getDFOutPtIdRef(srcLoc, srcVar)))
        {
            setVarDFInSetUpdated(dstLoc, dstVar);
            return true;
        }

        return false;
    }

    virtual inline bool updateDFOutFromIn(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        if (varHasNewDFInPts(srcLoc, srcVar))
        {
            removeVarFromDFInUpdatedSet(srcLoc, srcVar);
            if (this->unionPtsThroughIds(this->getDFOutPtIdRef(dstLoc, dstVar), this->getDFInPtIdRef(srcLoc, srcVar)))
            {
                setVarDFOutSetUpdated(dstLoc, dstVar);
                return true;
            }
        }

        return false;
    }

    virtual inline bool updateAllDFInFromOut(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        if (this->unionPtsThroughIds(this->getDFInPtIdRef(dstLoc, dstVar), this->getDFOutPtIdRef(srcLoc, srcVar)))
        {
            setVarDFInSetUpdated(dstLoc, dstVar);
            return true;
        }

        return false;
    }

    virtual inline bool updateAllDFInFromIn(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        if (this->unionPtsThroughIds(this->getDFInPtIdRef(dstLoc, dstVar), this->getDFInPtIdRef(srcLoc, srcVar)))
        {
            setVarDFInSetUpdated(dstLoc, dstVar);
            return true;
        }

        return false;
    }

    virtual inline bool updateAllDFOutFromIn(LocID loc, const Key& singleton, bool strongUpdates) override
    {
        bool changed = false;
        if (this->hasDFInSet(loc))
        {
            /// Only variables which have a new (IN) pts need to be updated.
            const KeySet vars = getDFInUpdatedVar(loc);
            for (const Key &var : vars)
            {
                /// Enable strong updates if it is required to do so
                if (strongUpdates && var == singleton) continue;
                if (updateDFOutFromIn(loc, var, loc, var)) changed = true;
            }
        }

        return changed;
    }

    virtual inline bool updateTLVPts(LocID srcLoc, const Key& srcVar, const Key& dstVar) override
    {
        if (varHasNewDFInPts(srcLoc, srcVar))
        {
            removeVarFromDFInUpdatedSet(srcLoc, srcVar);
            return this->unionPtsThroughIds(this->persPTData.ptsMap[dstVar], this->getDFInPtIdRef(srcLoc, srcVar));
        }

        return false;
    }

    virtual inline bool updateATVPts(const Key& srcVar, LocID dstLoc, const Key& dstVar) override
    {
        if (this->unionPtsThroughIds(this->getDFOutPtIdRef(dstLoc, dstVar), this->persPTData.ptsMap[srcVar]))
        {
            setVarDFOutSetUpdated(dstLoc, dstVar);
            return true;
        }

        return false;
    }

    virtual inline void clearAllDFOutUpdatedVar(LocID loc) override
    {
        if (this->hasDFOutSet(loc))
        {
            const KeySet vars = getDFOutUpdatedVar(loc);
            for (const Key &var : vars)
            {
                removeVarFromDFOutUpdatedSet(loc, var);
            }
        }
    }

    /// Methods to support type inquiry through isa, cast, and dyn_cast:
    ///@{
    static inline bool classof(const PersistentIncDFPTData<Key, KeySet, Data, DataSet> *)
    {
        return true;
    }

    static inline bool classof(const PTData<Key, KeySet, Data, DataSet> *ptd)
    {
        return ptd->getPTDTY() == BasePTData::PersIncDataFlow;
    }
    ///@}

private:

    /// Handle address-taken variables whose IN pts changed
    //@{
    /// Add var into loc's IN updated set. Called when var's pts in loc's IN set is changed.
    inline void setVarDFInSetUpdated(LocID loc, const Key& var)
    {
        insertKey<Key, KeySet>(var, inUpdatedVarMap[loc]);
    }

    /// Remove var from loc's IN updated set.
    inline void removeVarFromDFInUpdatedSet(LocID loc, const Key& var)
    {
        typename UpdatedVarMap::iterator it = inUpdatedVarMap.find(loc);
        if (it != inUpdatedVarMap.end()) it->second.reset(var);
    }

    /// Return TRUE if var has a new pts in loc's IN set
    inline bool varHasNewDFInPts(LocID loc, const Key& var)
    {
        typename UpdatedVarMap::iterator it = inUpdatedVarMap.find(loc);
        if (it != inUpdatedVarMap.end()) return it->second.test(var);
        return false;
    }

    /// Get all variables which have new pts informationin loc's IN set
    inline const KeySet& getDFInUpdatedVar(LocID loc)
    {
        return inUpdatedVarMap[loc];
    }
    ///@}

    /// Handle address-taken variables whose OUT pts changed
    ///@{
    /// Add var into loc's OUT updated set. Called when var's pts in loc's OUT set changed
    inline void setVarDFOutSetUpdated(LocID loc, const Key& var)
    {
        insertKey<Key, KeySet>(var, outUpdatedVarMap[loc]);
    }

    /// Remove var from loc's OUT updated set.
    inline void removeVarFromDFOutUpdatedSet(LocID loc, const Key& var)
    {
        typename UpdatedVarMap::iterator it = outUpdatedVarMap.find(loc);
        if (it != outUpdatedVarMap.end()) it->second.reset(var);
    }

    /// Return TRUE if var has a new pts in loc's OUT set.
    inline bool varHasNewDFOutPts(LocID loc, const Key& var)
    {
        typename UpdatedVarMap::iterator it = outUpdatedVarMap.find(loc);
        if (it != outUpdatedVarMap.end()) return it->second.test(var);
        return false;
    }

    /// Get all variables which have new pts info in loc's OUT set
    inline const KeySet& getDFOutUpdatedVar(LocID loc)
    {
        return outUpdatedVarMap[loc];
    }
    ///@}


private:
    UpdatedVarMap outUpdatedVarMap;
    UpdatedVarMap inUpdatedVarMap;
};

/// VersionedPTData implemented with persistent points-to sets (Data).
/// Implemented as a wrapper around two PersistentPTDatas: one for Keys, one
/// for VersionedKeys.
/// They are constructed with the same PersistentPointsToCache.
template <typename Key, typename KeySet, typename Data, typename DataSet, typename VersionedKey, typename VersionedKeySet>
class PersistentVersionedPTData : public VersionedPTData<Key, KeySet, Data, DataSet, VersionedKey, VersionedKeySet>
{
public:
    typedef PTData<Key, KeySet, Data, DataSet> BasePTData;
    typedef VersionedPTData<Key, KeySet, Data, DataSet, VersionedKey, VersionedKeySet> BaseVersionedPTData;
    typedef typename BasePTData::PTDataTy PTDataTy;

    typedef typename PersistentPTData<Key, KeySet, Data, DataSet>::KeyToIDMap KeyToIDMap;
    typedef typename PersistentPTData<VersionedKey, VersionedKeySet, Data, DataSet>::KeyToIDMap VersionedKeyToIDMap;

    PersistentVersionedPTData(PersistentPointsToCache<DataSet> &cache, bool reversePT = true, PTDataTy ty = PTDataTy::PersVersioned)
        : BaseVersionedPTData(reversePT, ty), tlPTData(cache, reversePT), atPTData(cache, reversePT) { }

    virtual ~PersistentVersionedPTData() { }

    virtual inline void clear() override
    {
        tlPTData.clear();
        atPTData.clear();
    }

    virtual const DataSet &getPts(const Key& vk) override
    {
        return tlPTData.getPts(vk);
    }
    virtual const DataSet &getPts(const VersionedKey& vk) override
    {
        return atPTData.getPts(vk);
    }

    virtual const KeySet& getRevPts(const Data &data) override
    {
        assert(this->rev && "PersistentVersionedPTData::getRevPts: constructed without reverse PT support!");
        return tlPTData.getRevPts(data);
    }
    virtual const VersionedKeySet& getVersionedKeyRevPts(const Data &data) override
    {
        assert(this->rev && "PersistentVersionedPTData::getVersionedKeyRevPts: constructed without reverse PT support!");
        return atPTData.getRevPts(data);
    }

    virtual bool addPts(const Key& k, const Data &element) override
    {
        return tlPTData.addPts(k, element);
    }
    virtual bool addPts(const VersionedKey& vk, const Data &element) override
    {
        return atPTData.addPts(vk, element);
    }

    virtual bool unionPts(const Key& dstVar, const Key& srcVar) override
    {
        return tlPTData.unionPts(dstVar, srcVar);
    }
    virtual bool unionPts(const VersionedKey& dstVar, const VersionedKey& srcVar) override
    {
        return atPTData.unionPts(dstVar, srcVar);
    }
    virtual bool unionPts(const VersionedKey& dstVar, const Key& srcVar) override
    {
        return atPTData.unionPtsFromId(dstVar, tlPTData.ptsMap[srcVar]);
    }
    virtual bool unionPts(const Key& dstVar, const VersionedKey& srcVar) override
    {
        return tlPTData.unionPtsFromId(dstVar, atPTData.ptsMap[srcVar]);
    }
    virtual bool unionPts(const Key &dstVar, const DataSet &srcData) override
    {
        return tlPTData.unionPts(dstVar, srcData);
    }
    virtual bool unionPts(const VersionedKey &dstVar, const DataSet &srcData) override
    {
        return atPTData.unionPts(dstVar, srcData);
    }

    virtual void clearPts(const Key& k, const Data &element) override
    {
        tlPTData.clearPts(k, element);
    }
    virtual void clearPts(const VersionedKey& vk, const Data &element) override
    {
        atPTData.clearPts(vk, element);
    }

    virtual void clearFullPts(const Key& k) override
    {
        tlPTData.clearFullPts(k);
    }
    virtual void clearFullPts(const VersionedKey& vk) override
    {
        atPTData.clearFullPts(vk);
    }

    virtual inline void dumpPTData() override
    {
        SVFUtil::outs() << "== Top-level points-to information\n";
        tlPTData.dumpPTData();
        SVFUtil::outs() << "== Address-taken points-to information\n";
        atPTData.dumpPTData();
    }

    /// Methods to support type inquiry through isa, cast, and dyn_cast:
    ///@{
    static inline bool classof(const PersistentVersionedPTData<Key, KeySet, Data, DataSet, VersionedKey, VersionedKeySet> *)
    {
        return true;
    }

    static inline bool classof(const PTData<Key, KeySet, Data, DataSet>* ptd)
    {
        return ptd->getPTDTY() == PTDataTy::PersVersioned;
    }
    ///@}

    virtual std::pair<u64_t, u64_t> topN(const unsigned n) const override
    {
        // TODO: create a helper...

        // How common are points-to sets across all maps and how many pointers are there?
        Map<PointsToID, u64_t> ptCounts;
        u64_t keys = 0;
        for (const typename KeyToIDMap::value_type &ki : tlPTData.ptsMap)
        {
            // Ignore empty points-to sets.
            if (ki.second != PersistentPointsToCache<DataSet>::emptyPointsToId())
            {
                ++ptCounts[ki.second];
                ++keys;
            }
        }

        for (const typename VersionedKeyToIDMap::value_type &vki : atPTData.ptsMap)
        {
            if (vki.second != PersistentPointsToCache<DataSet>::emptyPointsToId())
            {
                ++ptCounts[vki.second];
                ++keys;
            }
        }

        // Which points-to sets are the n most common?
        std::vector<u64_t> counts;
        for (const Map<PointsToID, u64_t>::value_type &ic : ptCounts) counts.push_back(ic.second);
        std::sort(counts.begin(), counts.end(), std::greater<>());

        // Sum of the counts of most common n points-to sets.
        u64_t mostCommonCount = 0;
        for (size_t i = 0; i < n && i < counts.size(); ++i) mostCommonCount += counts[i];

        return std::make_pair(mostCommonCount, keys);
    }

    virtual u64_t inUsePointsToSets(void) override
    {
        Set<PointsToID> pointsToSets;
        for (const typename KeyToIDMap::value_type &ki : tlPTData.ptsMap)
        {
            pointsToSets.insert(ki.second);
        }

        for (const typename VersionedKeyToIDMap::value_type &vki : atPTData.ptsMap)
        {
            pointsToSets.insert(vki.second);
        }

        return pointsToSets.size();
    }

private:
    /// PTData for Keys (top-level pointers, generally).
    PersistentPTData<Key, KeySet, Data, DataSet> tlPTData;
    /// PTData for VersionedKeys (address-taken objects, generally).
    PersistentPTData<VersionedKey, VersionedKeySet, Data, DataSet> atPTData;
};

} // End namespace SVF
#endif  // MUTABLE_POINTSTO_H_
