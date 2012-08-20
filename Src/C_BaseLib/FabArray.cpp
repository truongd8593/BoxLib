#include <winstd.H>

#include <FabArray.H>
#include <ParmParse.H>
//
// Set default values in Initialize()!!!
//
bool FabArrayBase::verbose;
bool FabArrayBase::do_async_sends;
bool FabArrayBase::do_not_use_cache;

namespace
{
    bool initialized = false;
    //
    // Set default values in Initialize()!!!
    //
    bool use_copy_cache;
    int  copy_cache_max_size;
    bool use_fb_cache;
    int  fb_cache_max_size;
}

void
FabArrayBase::Initialize ()
{
    if (initialized) return;
    //
    // Set default values here!!!
    //
    FabArrayBase::verbose          = true;
    FabArrayBase::do_async_sends   = false;
    FabArrayBase::do_not_use_cache = false;

    use_copy_cache      = true;
    copy_cache_max_size = 50;   // -1 ==> no maximum size
    use_fb_cache        = true;
    fb_cache_max_size   = 50;   // -1 ==> no maximum size

    ParmParse pp("fabarray");

    pp.query("verbose",          FabArrayBase::verbose);
    pp.query("do_async_sends",   FabArrayBase::do_async_sends);
    pp.query("do_not_use_cache", FabArrayBase::do_not_use_cache);

    pp.query("use_copy_cache",      use_copy_cache);
    pp.query("copy_cache_max_size", copy_cache_max_size);
    pp.query("use_fb_cache",        use_fb_cache);
    pp.query("fb_cache_max_size",   fb_cache_max_size);

    if (fb_cache_max_size <= 0 && fb_cache_max_size != -1)
        use_fb_cache = false;

    if (copy_cache_max_size <= 0 && copy_cache_max_size != -1)
        use_copy_cache = false;

    BoxLib::ExecOnFinalize(FabArrayBase::Finalize);

    initialized = true;
}

FabArrayBase::FabArrayBase ()
{
    Initialize();
}

FabArrayBase::~FabArrayBase () {}

const Box
FabArrayBase::fabbox (int K) const
{
    return BoxLib::grow(boxarray[K], n_grow);
}

//
// Used to cache some CommData stuff in CollectData().
//

FabArrayBase::CommDataCache::CommDataCache ()
    :
    m_valid(false)
{}

void
FabArrayBase::CommDataCache::operator= (const Array<ParallelDescriptor::CommData>& rhs)
{
    m_commdata = rhs;
    m_valid    = true;
}

//
// Stuff used for copy() caching.
//

FabArrayBase::CPC::CPC ()
    :
    m_reused(false) {}

FabArrayBase::CPC::CPC (const BoxArray&            dstba,
                        const BoxArray&            srcba,
                        const DistributionMapping& dstdm,
                        const DistributionMapping& srcdm)
    :
    m_dstba(dstba),
    m_srcba(srcba),
    m_dstdm(dstdm),
    m_srcdm(srcdm),
    m_reused(false) {}

FabArrayBase::CPC::~CPC () {}

bool
FabArrayBase::CPC::operator== (const CPC& rhs) const
{
    return
        m_dstba == rhs.m_dstba && m_srcba == rhs.m_srcba && m_dstdm == rhs.m_dstdm && m_srcdm == rhs.m_srcdm;
}

int
FabArrayBase::CPC::bytes () const
{
    //
    // Get a rough estimate on number of bytes used by a CPC.
    //
    int cnt = 0;

    cnt += m_LocTags.size()*sizeof(CopyComTag);

    for (CPC::MapOfCopyComTagContainers::const_iterator it = m_SndTags.begin(),
             m_End = m_SndTags.end();
         it != m_End;
         ++it)
    {
        cnt += it->second.size()*sizeof(CopyComTag);
    }

    for (CPC::MapOfCopyComTagContainers::const_iterator it = m_RcvTags.begin(),
             m_End = m_RcvTags.end();
         it != m_End;
         ++it)
    {
        cnt += it->second.size()*sizeof(CopyComTag);
    }

    cnt += 2*m_SndVols.size()*sizeof(int);
    cnt += 2*m_RcvVols.size()*sizeof(int);

    return cnt;
}

//
// The copy() cache.
//
FabArrayBase::CPCCache FabArrayBase::m_TheCopyCache;

FabArrayBase::CPCCacheIter
FabArrayBase::TheCPC (const CPC& cpc)
{
    const int Key = cpc.m_dstba.size() + cpc.m_srcba.size();

    if (use_copy_cache)
    {
        std::pair<CPCCacheIter,CPCCacheIter> er_it = m_TheCopyCache.equal_range(Key);

        for (CPCCacheIter it = er_it.first; it != er_it.second; ++it)
        {
            if (it->second->operator==(cpc))
            {
                it->second->m_reused = true;

                return it;
            }
        }

        if (m_TheCopyCache.size() >= copy_cache_max_size && copy_cache_max_size != -1)
        {
            //
            // Don't let the size of the cache get too big.
            //
            for (CPCCacheIter it = m_TheCopyCache.begin(); it != m_TheCopyCache.end(); )
            {
                if (!it->second->m_reused)
                {
                    //
                    // Don't forget to delete the pointer!
                    //
                    delete it->second;

                    m_TheCopyCache.erase(it++);

                    if (m_TheCopyCache.size() < copy_cache_max_size)
                        //
                        // Only delete enough entries to stay under limit.
                        //
                        break;
                }
                else
                {
                    ++it;
                }
            }

            if (m_TheCopyCache.size() >= copy_cache_max_size)
            {
                //
                // Get rid of first entry which is the one with the smallest key.
                //
                CPCCacheIter it = m_TheCopyCache.begin();

                if (it != m_TheCopyCache.end())
                {
                    //
                    // Don't forget to delete the pointer!
                    //
                    delete it->second;

                    m_TheCopyCache.erase(it);
                }
            }
        }
    }
    else
    {
        m_TheCopyCache.clear();
    }
    //
    // Got to build one.  Here's where we allocate memory for the cache.
    //
    const int    MyProc   = ParallelDescriptor::MyProc();
    CPCCacheIter cache_it = m_TheCopyCache.insert(std::make_pair(Key,new CPC(cpc)));
    CPC&         TheCPC   = *cache_it->second;

    CopyComTag                        tag;
    std::vector< std::pair<int,Box> > isects;
    std::map<int,int>::iterator       vol_it;

    for (int i = 0, N = TheCPC.m_dstba.size(); i < N; i++)
    {
        TheCPC.m_srcba.intersections(TheCPC.m_dstba[i],isects);

        const int d_owner = TheCPC.m_dstdm[i];

        for (int j = 0, M = isects.size(); j < M; j++)
        {
            const Box& bx      = isects[j].second;
            const int  k       = isects[j].first;
            const int  s_owner = TheCPC.m_srcdm[k];

            if (d_owner != MyProc && s_owner != MyProc) continue;

            tag.box = bx;

            const int vol = bx.numPts();

            if (d_owner == MyProc)
            {
                tag.fabIndex = i;

                if (s_owner == MyProc)
                {
                    tag.srcIndex = k;

                    TheCPC.m_LocTags.push_back(tag);
                }
                else
                {
                    TheCPC.m_RcvTags[s_owner].push_back(tag);

                    vol_it = TheCPC.m_RcvVols.find(s_owner);

                    if (vol_it != TheCPC.m_RcvVols.end())
                    {
                        vol_it->second += vol;
                    }
                    else
                    {
                        TheCPC.m_RcvVols[s_owner] = vol;
                    }
                }
            }
            else if (s_owner == MyProc)
            {
                tag.fabIndex = k;

                TheCPC.m_SndTags[d_owner].push_back(tag);

                vol_it = TheCPC.m_SndVols.find(d_owner);

                if (vol_it != TheCPC.m_SndVols.end())
                {
                    vol_it->second += vol;
                }
                else
                {
                    TheCPC.m_SndVols[d_owner] = vol;
                }
            }
        }
    }

    if (TheCPC.m_LocTags.empty() && TheCPC.m_SndTags.empty() && TheCPC.m_RcvTags.empty())
    {
        //
        // This MPI proc has no work to do.
        // Don't store in the cache.
        // And don't forget to delete the pointer!
        //
        delete cache_it->second;

        m_TheCopyCache.erase(cache_it);

        return m_TheCopyCache.end();
    }

    return cache_it;
}

void
FabArrayBase::CPC::FlushCache ()
{
    int stats[3] = {0,0,0}; // size, reused, bytes

    stats[0] = m_TheCopyCache.size();

    for (CPCCacheIter it = m_TheCopyCache.begin(),
             End = m_TheCopyCache.end();
         it != End;
         ++it)
    {
        stats[2] += it->second->bytes();
        if (it->second->m_reused)
            stats[1]++;
        //
        // Don't forget to delete the pointer!
        //
        delete it->second;
    }

    if (FabArrayBase::verbose)
    {
        ParallelDescriptor::ReduceIntMax(&stats[0], 3, ParallelDescriptor::IOProcessorNumber());

        if (stats[0] > 0 && ParallelDescriptor::IOProcessor())
        {
            std::cout << "CPC::m_TheCopyCache: max size: "
                      << stats[0]
                      << ", max # reused: "
                      << stats[1]
                      << ", max bytes used: "
                      << stats[2]
                      << std::endl;
        }
    }

    m_TheCopyCache.clear();
}

FabArrayBase::SI::SI ()
    :
    m_ngrow(-1),
    m_cross(false),
    m_reused(false) {}

FabArrayBase::SI::SI (const BoxArray&            ba,
                      const DistributionMapping& dm,
                      int                        ngrow,
                      bool                       cross)
    :
    m_ba(ba),
    m_dm(dm),
    m_ngrow(ngrow),
    m_cross(cross),
    m_reused(false)
{
    BL_ASSERT(ngrow >= 0);
}

FabArrayBase::SI::~SI () {}

bool
FabArrayBase::SI::operator== (const SI& rhs) const
{
    return
        m_ngrow == rhs.m_ngrow && m_cross == rhs.m_cross && m_ba == rhs.m_ba && m_dm == rhs.m_dm;
}

int
FabArrayBase::SI::bytes () const
{
    //
    // Get a rough estimate of number of bytes used by a CPC.
    //
    int cnt = 0;

    cnt += m_LocTags.size()*sizeof(CopyComTag);

    for (CPC::MapOfCopyComTagContainers::const_iterator it = m_SndTags.begin(),
             m_End = m_SndTags.end();
         it != m_End;
         ++it)
    {
        cnt += it->second.size()*sizeof(CopyComTag);
    }

    for (CPC::MapOfCopyComTagContainers::const_iterator it = m_RcvTags.begin(),
             m_End = m_RcvTags.end();
         it != m_End;
         ++it)
    {
        cnt += it->second.size()*sizeof(CopyComTag);
    }

    cnt += 2*m_SndVols.size()*sizeof(int);
    cnt += 2*m_RcvVols.size()*sizeof(int);

    return cnt;
}

FabArrayBase::FBCache FabArrayBase::m_TheFBCache;

void
FabArrayBase::Finalize ()
{
    FabArrayBase::FlushSICache();
    FabArrayBase::CPC::FlushCache();

    initialized = false;
}

void
FabArrayBase::FlushSICache ()
{
    int stats[3] = {0,0,0}; // size, reused, bytes

    stats[0] = m_TheFBCache.size();

    for (FBCacheIter it = m_TheFBCache.begin(),
             End = m_TheFBCache.end();
         it != End;
         ++it)
    {
        stats[2] += it->second->bytes();
        if (it->second->m_reused)
            stats[1]++;
        //
        // Don't forget to delete the pointer!
        //
        delete it->second;
    }

    if (FabArrayBase::verbose)
    {
        ParallelDescriptor::ReduceIntMax(&stats[0], 3, ParallelDescriptor::IOProcessorNumber());

        if (stats[0] > 0 && ParallelDescriptor::IOProcessor())
        {
            std::cout << "SI::TheFBCache: max size: "
                      << stats[0]
                      << ", max # reused: "
                      << stats[1]
                      << ", max bytes used: "
                      << stats[2]
                      << std::endl;
        }
    }

    m_TheFBCache.clear();
}

int
FabArrayBase::SICacheSize ()
{
    return m_TheFBCache.size();
}

FabArrayBase::FBCacheIter
FabArrayBase::TheFB (bool                cross,
                     const FabArrayBase& mf)
{
    const FabArrayBase::SI si(mf.boxArray(), mf.DistributionMap(), mf.nGrow(), cross);

    const int Key = mf.size() + mf.nGrow() + cross;

    if (use_fb_cache)
    {
        std::pair<FBCacheIter,FBCacheIter> er_it = m_TheFBCache.equal_range(Key);
    
        for (FBCacheIter it = er_it.first; it != er_it.second; ++it)
        {
            if (it->second->operator==(si))
            {
                it->second->m_reused = true;

                return it;
            }
        }

        if (m_TheFBCache.size() >= fb_cache_max_size && fb_cache_max_size != -1)
        {
            //
            // Don't let the size of the cache get too big.
            //
            for (FBCacheIter it = m_TheFBCache.begin(); it != m_TheFBCache.end(); )
            {
                if (!it->second->m_reused)
                {
                    //
                    // Don't forget to delete the pointer!
                    //
                    delete it->second;

                    m_TheFBCache.erase(it++);

                    if (m_TheFBCache.size() < fb_cache_max_size)
                        //
                        // Only delete enough entries to stay under limit.
                        //
                        break;
                }
                else
                {
                    ++it;
                }
            }

            if (m_TheFBCache.size() >= fb_cache_max_size)
            {
                //
                // Get rid of first entry which is the one with the smallest key.
                //
                FBCacheIter it = m_TheFBCache.begin();

                if (it != m_TheFBCache.end())
                {
                    //
                    // Don't forget to delete the pointer!
                    //
                    delete it->second;

                    m_TheFBCache.erase(it);
                }

            }
        }
    }
    else
    {
        m_TheFBCache.clear();
    }
    //
    // Got to build one.
    //
    FBCacheIter                cache_it = m_TheFBCache.insert(std::make_pair(Key,new SI(si)));
    const BoxArray&            ba       = mf.boxArray();
    const DistributionMapping& dm       = mf.DistributionMap();
    const int                  MyProc   = ParallelDescriptor::MyProc();
    SI&                        theFB    = *cache_it->second;

    CopyComTag                        tag;
    std::vector<Box>                  boxes;
    std::vector< std::pair<int,Box> > isects;
    std::map<int,int>::iterator       vol_it;

    boxes.resize(si.m_cross ? 2*BL_SPACEDIM : 1);

    for (int i = 0, N = ba.size(); i < N; i++)
    {
        if (si.m_cross)
        {
            const Box& vbx = ba[i];

            for (int dir = 0; dir < BL_SPACEDIM; dir++)
            {
                Box lo = vbx;
                lo.setSmall(dir, vbx.smallEnd(dir) - si.m_ngrow);
                lo.setBig  (dir, vbx.smallEnd(dir) - 1);
                boxes[2*dir+0] = lo;

                Box hi = vbx;
                hi.setSmall(dir, vbx.bigEnd(dir) + 1);
                hi.setBig  (dir, vbx.bigEnd(dir) + si.m_ngrow);
                boxes[2*dir+1] = hi;
            }
        }
        else
        {
            boxes[0] = BoxLib::grow(ba[i],si.m_ngrow);
        }

        const int d_owner = dm[i];

        for (std::vector<Box>::const_iterator it = boxes.begin(),
                 End = boxes.end();
             it != End;
             ++it)
        {
            ba.intersections(*it,isects);

            for (int j = 0, M = isects.size(); j < M; j++)
            {
                const Box& bx      = isects[j].second;
                const int  k       = isects[j].first;
                const int  s_owner = dm[k];

                if (d_owner != MyProc && s_owner != MyProc) continue;

                if (k == i) continue;

                const int vol = bx.numPts();

                tag.box = bx;

                if (d_owner == MyProc)
                {
                    tag.fabIndex = i;

                    if (s_owner == MyProc)
                    {
                        tag.srcIndex = k;

                        theFB.m_LocTags.push_back(tag);
                    }
                    else
                    {
                        theFB.m_RcvTags[s_owner].push_back(tag);

                        vol_it = theFB.m_RcvVols.find(s_owner);

                        if (vol_it != theFB.m_RcvVols.end())
                        {
                            vol_it->second += vol;
                        }
                        else
                        {
                            theFB.m_RcvVols[s_owner] = vol;
                        }
                    }
                }
                else if (s_owner == MyProc)
                {
                    tag.fabIndex = k;

                    theFB.m_SndTags[d_owner].push_back(tag);

                    vol_it = theFB.m_SndVols.find(d_owner);

                    if (vol_it != theFB.m_SndVols.end())
                    {
                        vol_it->second += vol;
                    }
                    else
                    {
                        theFB.m_SndVols[d_owner] = vol;
                    }
                }
            }
        }
    }

    if (theFB.m_LocTags.empty() && theFB.m_SndTags.empty() && theFB.m_RcvTags.empty())
    {
        //
        // This MPI proc has no work to do.
        // Don't store in the cache.
        // And don't forget to delete the pointer!
        //
        delete cache_it->second;

        m_TheFBCache.erase(cache_it);

        return m_TheFBCache.end();
    }

    return cache_it;
}
