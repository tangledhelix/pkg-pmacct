#if defined __PMACCTD_C || defined __NFACCTD_C || defined __SFACCTD_C 
#define EXT 
#else
#define EXT extern
#endif

EXT u_int32_t PdataSz, ChBufHdrSz, CharPtrSz, CounterSz;
EXT u_int32_t NfHdrV5Sz, NfHdrV1Sz, NfHdrV7Sz, NfHdrV8Sz, NfHdrV9Sz;
EXT u_int32_t NfDataHdrV9Sz, NfTplHdrV9Sz;
EXT u_int32_t NfDataV1Sz, NfDataV5Sz, NfDataV7Sz;
EXT u_int32_t IP4HdrSz, IP4TlSz, IP6HdrSz, IP6AddrSz, IP6TlSz; 
EXT u_int32_t MyTLHdrSz, TCPFlagOff;
EXT u_int32_t SFSampleSz, SFLAddressSz, SFrenormEntrySz;
EXT u_int32_t PptrsSz, UDPHdrSz, CSSz, MyTCPHdrSz, IpFlowCmnSz; 

#undef EXT