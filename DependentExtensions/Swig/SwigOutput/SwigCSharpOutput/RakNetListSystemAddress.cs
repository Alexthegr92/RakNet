//------------------------------------------------------------------------------
// <auto-generated />
//
// This file was automatically generated by SWIG (http://www.swig.org).
// Version 3.0.10
//
// Do not make changes to this file unless you know what you are doing--modify
// the SWIG interface file instead.
//------------------------------------------------------------------------------

namespace RakNet {

public class RakNetListSystemAddress : global::System.IDisposable {
  private global::System.Runtime.InteropServices.HandleRef swigCPtr;
  protected bool swigCMemOwn;

  internal RakNetListSystemAddress(global::System.IntPtr cPtr, bool cMemoryOwn) {
    swigCMemOwn = cMemoryOwn;
    swigCPtr = new global::System.Runtime.InteropServices.HandleRef(this, cPtr);
  }

  internal static global::System.Runtime.InteropServices.HandleRef getCPtr(RakNetListSystemAddress obj) {
    return (obj == null) ? new global::System.Runtime.InteropServices.HandleRef(null, global::System.IntPtr.Zero) : obj.swigCPtr;
  }

  ~RakNetListSystemAddress() {
    Dispose();
  }

  public virtual void Dispose() {
    lock(this) {
      if (swigCPtr.Handle != global::System.IntPtr.Zero) {
        if (swigCMemOwn) {
          swigCMemOwn = false;
          RakNetPINVOKE.delete_RakNetListSystemAddress(swigCPtr);
        }
        swigCPtr = new global::System.Runtime.InteropServices.HandleRef(null, global::System.IntPtr.Zero);
      }
      global::System.GC.SuppressFinalize(this);
    }
  }

    public SystemAddress this[int index]  
    {  
        get   
        {
            return Get((uint)index); // use indexto retrieve and return another value.    
        }  
        set   
        {
            Replace(value, value, (uint)index, "Not used", 0);// use index and value to set the value somewhere.   
        }  
    } 

  public RakNetListSystemAddress() : this(RakNetPINVOKE.new_RakNetListSystemAddress__SWIG_0(), true) {
  }

  public RakNetListSystemAddress(RakNetListSystemAddress original_copy) : this(RakNetPINVOKE.new_RakNetListSystemAddress__SWIG_1(RakNetListSystemAddress.getCPtr(original_copy)), true) {
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
  }

  public RakNetListSystemAddress CopyData(RakNetListSystemAddress original_copy) {
    RakNetListSystemAddress ret = new RakNetListSystemAddress(RakNetPINVOKE.RakNetListSystemAddress_CopyData(swigCPtr, RakNetListSystemAddress.getCPtr(original_copy)), false);
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
    return ret;
  }

  public SystemAddress Get(uint position) {
    SystemAddress ret = new SystemAddress(RakNetPINVOKE.RakNetListSystemAddress_Get(swigCPtr, position), false);
    return ret;
  }

  public void Push(SystemAddress input, string file, uint line) {
    RakNetPINVOKE.RakNetListSystemAddress_Push(swigCPtr, SystemAddress.getCPtr(input), file, line);
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
  }

  public SystemAddress Pop() {
    SystemAddress ret = new SystemAddress(RakNetPINVOKE.RakNetListSystemAddress_Pop(swigCPtr), false);
    return ret;
  }

  public void Insert(SystemAddress input, uint position, string file, uint line) {
    RakNetPINVOKE.RakNetListSystemAddress_Insert__SWIG_0(swigCPtr, SystemAddress.getCPtr(input), position, file, line);
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
  }

  public void Insert(SystemAddress input, string file, uint line) {
    RakNetPINVOKE.RakNetListSystemAddress_Insert__SWIG_1(swigCPtr, SystemAddress.getCPtr(input), file, line);
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
  }

  public void Replace(SystemAddress input, SystemAddress filler, uint position, string file, uint line) {
    RakNetPINVOKE.RakNetListSystemAddress_Replace__SWIG_0(swigCPtr, SystemAddress.getCPtr(input), SystemAddress.getCPtr(filler), position, file, line);
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
  }

  public void Replace(SystemAddress input) {
    RakNetPINVOKE.RakNetListSystemAddress_Replace__SWIG_1(swigCPtr, SystemAddress.getCPtr(input));
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
  }

  public void RemoveAtIndex(uint position) {
    RakNetPINVOKE.RakNetListSystemAddress_RemoveAtIndex(swigCPtr, position);
  }

  public void RemoveAtIndexFast(uint position) {
    RakNetPINVOKE.RakNetListSystemAddress_RemoveAtIndexFast(swigCPtr, position);
  }

  public void RemoveFromEnd(uint num) {
    RakNetPINVOKE.RakNetListSystemAddress_RemoveFromEnd__SWIG_0(swigCPtr, num);
  }

  public void RemoveFromEnd() {
    RakNetPINVOKE.RakNetListSystemAddress_RemoveFromEnd__SWIG_1(swigCPtr);
  }

  public uint GetIndexOf(SystemAddress input) {
    uint ret = RakNetPINVOKE.RakNetListSystemAddress_GetIndexOf(swigCPtr, SystemAddress.getCPtr(input));
    if (RakNetPINVOKE.SWIGPendingException.Pending) throw RakNetPINVOKE.SWIGPendingException.Retrieve();
    return ret;
  }

  public uint Size() {
    uint ret = RakNetPINVOKE.RakNetListSystemAddress_Size(swigCPtr);
    return ret;
  }

  public void Clear(bool doNotDeallocateSmallBlocks, string file, uint line) {
    RakNetPINVOKE.RakNetListSystemAddress_Clear(swigCPtr, doNotDeallocateSmallBlocks, file, line);
  }

  public void Preallocate(uint countNeeded, string file, uint line) {
    RakNetPINVOKE.RakNetListSystemAddress_Preallocate(swigCPtr, countNeeded, file, line);
  }

  public void Compress(string file, uint line) {
    RakNetPINVOKE.RakNetListSystemAddress_Compress(swigCPtr, file, line);
  }

}

}
