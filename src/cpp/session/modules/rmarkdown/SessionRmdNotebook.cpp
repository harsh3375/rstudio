/*
 * SessionRmdNotebook.cpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionRmdNotebook.hpp"
#include "NotebookPlots.hpp"
#include "NotebookPlotReplay.hpp"
#include "NotebookCache.hpp"
#include "NotebookChunkDefs.hpp"
#include "NotebookOutput.hpp"
#include "NotebookHtmlWidgets.hpp"
#include "NotebookExec.hpp"
#include "NotebookErrors.hpp"
#include "NotebookQueue.hpp"

#include <iostream>

#include <boost/foreach.hpp>
#include <boost/format.hpp>

#include <r/RJson.hpp>
#include <r/RExec.hpp>

#include <core/Exec.hpp>
#include <core/Algorithm.hpp>
#include <core/json/Json.hpp>
#include <core/json/JsonRpc.hpp>
#include <core/StringUtils.hpp>
#include <core/system/System.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/SessionOptions.hpp>
#include <session/SessionUserSettings.hpp>

#define kFinishedReplay      0
#define kFinishedInteractive 1

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {

namespace {

// the currently active console ID and chunk execution context
std::string s_activeConsole;
boost::shared_ptr<ChunkExecContext> s_execContext;

void replayChunkOutputs(const std::string& docPath, const std::string& docId,
      const std::string& requestId, const json::Array& chunkOutputs) 
{
   std::vector<std::string> chunkIds;
   extractChunkIds(chunkOutputs, &chunkIds);

   // find all the chunks and play them back to the client
   BOOST_FOREACH(const std::string& chunkId, chunkIds)
   {
      enqueueChunkOutput(docPath, docId, chunkId, notebookCtxId(), requestId);
   }

   json::Object result;
   result["doc_id"] = docId;
   result["request_id"] = requestId;
   result["chunk_id"] = "";
   result["type"] = kFinishedReplay;
   ClientEvent event(client_events::kChunkOutputFinished, result);
   module_context::enqueClientEvent(event);
}

// called by the client to inject output into a recently opened document 
Error refreshChunkOutput(const json::JsonRpcRequest& request,
                         json::JsonRpcResponse* pResponse)
{
   // extract path to doc to be refreshed
   std::string docPath, docId, nbCtxId, requestId;
   Error error = json::readParams(request.params, &docPath, &docId, &nbCtxId,
         &requestId);
   if (error)
      return error;

   json::Object result;
   json::Value chunkDefs; 

   // use our own context ID if none supplied
   if (nbCtxId.empty())
      error = getChunkDefs(docPath, docId, NULL, &chunkDefs);
   else
      error = getChunkDefs(docPath, docId, nbCtxId, NULL, &chunkDefs);

   // schedule the work to play back the chunks
   if (!error && chunkDefs.type() == json::ArrayType) 
   {
      pResponse->setAfterResponse(
            boost::bind(replayChunkOutputs, docPath, docId, requestId, 
                        chunkDefs.get_array()));
   }

   // send back the execution queue, if any
   pResponse->setResult(getDocQueue(docId));

   return Success();
}

void emitOutputFinished(const std::string& docId, const std::string& chunkId,
      int scope)
{
   json::Object result;
   result["doc_id"]     = docId;
   result["request_id"] = "";
   result["chunk_id"]   = chunkId;
   result["type"]       = kFinishedInteractive;
   result["scope"]      = scope;
   ClientEvent event(client_events::kChunkOutputFinished, result);
   module_context::enqueClientEvent(event);
}

void onActiveConsoleChanged(const std::string& consoleId, 
                            const std::string& text)
{
   s_activeConsole = consoleId;
   if (!s_execContext)
      return;

   if (consoleId == s_execContext->chunkId())
   {
      if (s_execContext->connected()) 
         return;
      s_execContext->connect();
      s_execContext->onConsoleInput(text);
   }
   else if (s_execContext->connected())
   {
      s_execContext->disconnect();
      s_execContext.reset();
   }
}

void onChunkExecCompleted(const std::string& docId, 
                          const std::string& chunkId,
                          const std::string& nbCtxId)
{
   // if this event belonged to the current execution context, destroy it
   if (s_execContext &&
       s_execContext->docId() == docId &&
       s_execContext->chunkId() == chunkId)
   {
      emitOutputFinished(docId, chunkId, s_execContext->execScope());
      s_execContext.reset();
   }
   else
   {
      emitOutputFinished(docId, chunkId, ExecScopeChunk);
   }
}

// called by the client to set the active chunk console
Error setChunkConsole(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   std::string docId, chunkId, options;
   int pixelWidth = 0, charWidth = 0, execMode = 0, execScope = 0,
       commitMode = 0;
   Error error = json::readParams(request.params, &docId, &chunkId, &commitMode,
         &execMode, &execScope, &options, &pixelWidth, &charWidth);
   if (error)
      return error;

   // evaluate this chunk's options
   r::sexp::Protect protect;
   SEXP sexpOptions = R_NilValue;
   error = r::exec::RFunction(".rs.evaluateChunkOptions", options)
                             .call(&sexpOptions, &protect);
   if (error)
      return error;

   // convert to JSON for client
   json::Value jsonOptions;
   error = r::json::jsonValueFromList(sexpOptions, &jsonOptions);
   if (error)
      return error;
   pResponse->setResult(jsonOptions);

   // if this chunk is going to be evaluated in batch mode, and the options
   // indicate that it shouldn't be evaluated, don't
   // evaluate it
   if (execMode == ExecModeBatch &&
       jsonOptions.type() == json::ObjectType)
   {
      bool eval = true;
      error = json::readObject(jsonOptions.get_obj(), "eval", &eval);
      if (!error && !eval) 
      {
         return Success();
      }
   }

   // clean up the old execution context if we still have one
   if (s_execContext)
      s_execContext->disconnect();

   // choose appropriate notebook context to write to -- if this is a saved
   // Rmd, we'll write directly to the saved context
   std::string nbCtxId = 
       static_cast<CommitMode>(commitMode) == ModeCommitted ?
         kSavedCtx :
         notebookCtxId();

   const json::Object& optionsJson = jsonOptions.type() == json::ObjectType ? 
      jsonOptions.get_obj() : json::Object();
   s_execContext.reset(new ChunkExecContext(docId, chunkId, nbCtxId, 
            static_cast<ExecScope>(execScope), optionsJson, pixelWidth, 
            charWidth));
   if (s_activeConsole == chunkId)
      s_execContext->connect();

   return Success();
}

} // anonymous namespace

Events& events()
{
   static Events instance;
   return instance;
}

// a notebook context is scoped to both a user and a session (which are only
// guaranteed unique per user); it must be unique since there are currently no
// concurrency mechanisms in place to guard multi-session writes to the file.
// the notebook context ID may be shared with other users/sessions for read 
// access during collaborative editing, but only a notebook context's own 
// session should write to it.
std::string notebookCtxId()
{
   return userSettings().contextId() + module_context::activeSession().id();
}

Error initialize()
{
   using boost::bind;
   using namespace module_context;

   module_context::events().onActiveConsoleChanged.connect(
         onActiveConsoleChanged);

   events().onChunkExecCompleted.connect(onChunkExecCompleted);

   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "refresh_chunk_output", refreshChunkOutput))
      (bind(registerRpcMethod, "set_chunk_console", setChunkConsole))
      (bind(module_context::sourceModuleRFile, "SessionRmdNotebook.R"))
      (bind(initOutput))
      (bind(initCache))
      (bind(initHtmlWidgets))
      (bind(initErrors))
      (bind(initPlots))
      (bind(initPlotReplay))
      (bind(initQueue));

   return initBlock.execute();
}

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio

