#include "platform/platform.h"
#include "console/console.h"

#include "console/codeblockEvalState.h"
#include "console/consoleNamespace.h"

#include "console/ast.h"
#include "collection/findIterator.h"
#include "io/resource/resourceManager.h"

#include "string/findMatch.h"
#include "io/fileStream.h"
#include "console/compiler.h"

#include "codeblockEvalState_ScriptBinding.h"

#include "debug/telnetDebugger.h"
#include "debug/remote/RemoteDebuggerBase.h"


void CodeBlockEvalState::pushFunction(CodeBlockFunction* function, CodeBlock* code, Namespace::Entry* entry, U32 numParams)
{
   if (frames.size() == 0)
   {
      currentFrame.isRoot = true;
      currentFrame.localVars = NULL;
   }
   else
   {
      currentFrame.isRoot = false;
   }
   
   frames.push_back(currentFrame);
   
   currentFrame.localVars = NULL;
   currentFrame.function = function;
   currentFrame.filename = code->modPath;
   currentFrame.savedIP = function->ip;
   currentFrame.code   = CodeBlock::smCurrentCodeBlock = code;
   currentFrame.constants = code->mConstants.address();
   currentFrame.constantTop = 0; // resets to page 0 for new functions
   
   //Con::printf("Pushing codeblock %s", currentFrame.code->fullPath);
   
   if (entry)
   {
      currentFrame.package = entry->mPackage;
      currentFrame.ns = entry->mNamespace->mName;
   }
   
   // As an optimization we only increment the stack to returnStart, and
   // blank out any unused vars. This means any register slots after will get
   // trashed (though there shouldn't be any anyway!).
   U32 usedVars = numParams > function->numArgs ? function->numArgs : numParams; //nsEntry->mMaxArgs ? nsEntry->mMaxArgs : numParams;
   
   if (currentFrame.isRoot)
   {
      currentFrame.stackTop = 0;
   }
   else
   {
      currentFrame.stackTop += currentFrame.returnReg+1; // skip function name, we dont need it
      currentFrame.isRoot = false;
   }
   
   U32 requiredStackSize = currentFrame.stackTop + function->maxStack;
   if (stack.size() < requiredStackSize)
   {
      U32 startSize = stack.size();
      stack.setSize(stack.size() + requiredStackSize);
      for (U32 i=startSize; i<startSize+requiredStackSize; i++)
      {
         stack[i].type = ConsoleValue::TypeInternalNull;
      }
   }
   
   ConsoleValuePtr *const base = stack.address() + currentFrame.stackTop;
   
   for (U32 j=numParams; j<function->numArgs; j++)
   {
      base[currentFrame.returnReg+j].setNull();
   }
   
   // Grab the state of the telenet debugger here once
   // so that the push and pop frames are always balanced.
   const bool telDebuggerOn = TelDebugger && TelDebugger->isConnected();
   if ( telDebuggerOn )
      TelDebugger->pushStackFrame();
   
   // Notify the remote debugger.
   RemoteDebuggerBase* pRemoteDebugger = RemoteDebuggerBase::getRemoteDebugger();
   if ( pRemoteDebugger != NULL )
      pRemoteDebugger->pushStackFrame();
   
   
   // Trace handler
   if(traceOn)
   {
      char traceBuffer[4096];
      traceBuffer[0] = 0;
      dStrcat(traceBuffer, "Entering ");
      if(currentFrame.package)
      {
         dStrcat(traceBuffer, "[");
         dStrcat(traceBuffer, currentFrame.package);
         dStrcat(traceBuffer, "]");
      }
      if(currentFrame.ns)
      {
         dSprintf(traceBuffer + dStrlen(traceBuffer), sizeof(traceBuffer) - dStrlen(traceBuffer),
                  "%s::%s(", currentFrame.ns, function->name);
      }
      else
      {
         dSprintf(traceBuffer + dStrlen(traceBuffer), sizeof(traceBuffer) - dStrlen(traceBuffer),
                  "%s(", function->name);
      }
      for(U32 i = 0; i < numParams; i++)
      {
         dStrcat(traceBuffer, base[currentFrame.returnReg+i].getTempStringValue());
         if(i != numParams - 1)
            dStrcat(traceBuffer, ", ");
      }
      dStrcat(traceBuffer, ")");
      Con::printf("%s", traceBuffer);
   }
}

void CodeBlockEvalState::popFunction()
{
   AssertFatal(frames.size() > 0, "Stack misbalance");
   
   //Con::printf("Popping codeblock %s", currentFrame.code->fullPath);
   
   // Copy locals to dictionary if applicable
   if (currentFrame.localVars)
   {
      if (!currentFrame.localVars->isRoot())
      {
         copyFrameToLocals(currentFrame.localVars, &currentFrame);
      }
      disposeLocals(currentFrame.localVars);
   }
   
   currentFrame = frames[frames.size()-1];
   frames.pop_back();
   //Con::printf("Codeblock now %s", currentFrame.code->fullPath);
   
   CodeBlock::smCurrentCodeBlock = currentFrame.code;
   
   const bool telDebuggerOn = TelDebugger && TelDebugger->isConnected();
   
   if ( telDebuggerOn )
      TelDebugger->popStackFrame();
   
   // Notify the remote debugger.
   RemoteDebuggerBase* pRemoteDebugger = RemoteDebuggerBase::getRemoteDebugger();
   if ( pRemoteDebugger != NULL )
      pRemoteDebugger->popStackFrame();
}


Dictionary *CodeBlockEvalState::createLocals(Dictionary* base)
{
   return new Dictionary(this, base);
}

void CodeBlockEvalState::copyFrameToLocals(Dictionary* locals, InternalState* srcFrame)
{
   CodeBlockFunction* func = srcFrame->function;
   if (!func)
      return;
   
   for (U32 i=0, sz=func->vars.size(); i<sz; i++)
   {
      CodeBlockFunction::Symbol &symbol = func->vars[i];
      //Con::printf("copyFrame(%s) = REG %i (abs %i)", symbol.varName, symbol.registerIdx, srcFrame->stackTop + symbol.registerIdx);
      locals->setValueVariable(symbol.varName, this->stack[srcFrame->stackTop + symbol.registerIdx]);
   }
}

void CodeBlockEvalState::copyLocalsToFrame(Dictionary* locals, InternalState* dstFrame)
{
   CodeBlockFunction* func = dstFrame->function;
   if (!func)
      return;
   
   locals->setFrame(func, stack.address()+dstFrame->stackTop);
}

void CodeBlockEvalState::disposeLocals(Dictionary* locals)
{
   delete locals;
}

