//
// <copyright file="ComputationNetworkEvaluation.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "Basics.h"
#include "ComputationNode.h"
#include "ComputationNetwork.h"
#include "RecurrentNodes.h"
#include <string>
#include <vector>
#include <list>
#include <set>

using namespace std;

namespace Microsoft { namespace MSR { namespace CNTK {

    // This source file contains methods related to evaluation (forward prop, backprop), network validation, and matrix memory allocation (memory sharing).

    // -----------------------------------------------------------------------
    // evaluation
    // -----------------------------------------------------------------------

    // MAIN ENTRY POINT for evaluating one minibatch (forward prop)
    // TODO: pass a set of nodes instead of only one
    // TODO: rename to ForwardProp()? To make it very clear?
    // This calls EvaluateThisNode() on all nodes in order of data flow through the network.
    // By default, the network is applied concurrently on all frames in a minibatch in parallel (a "map" operation)
    // Recurrent loops deviate:
    //  - a recurrent loop is the loop of nodes that make up computation for one time step (e.g. Times -> Plus -> Sigmoid -> Delay)
    //  - these must be executed frame by frame rather than as a map
    //  - such a loop is treated as if they were a little nested network; this is done inside here
    //  - these little nested networks are defined in m_recurrentInfo[]
    void ComputationNetwork::Evaluate(const ComputationNodeBasePtr & rootNode)
    {
        // caller must call BuildAndValidateSubNetwork() before
        // TODO: Some places are hard to fix, e.g. encoder-decoder best-path functions. Those may be broken; this message will tell you.
        if (!BuiltAndValidatedSubNetwork(rootNode))
            LogicError("Evaluate for node %ls %ls: BuildAndValidateSubNetwork() has not been called on this node.");

        // determines order of evaluation, such that children get evaluated before their parent nodes
        std::list<ComputationNodeBasePtr>& allNodes = GetEvalOrder(rootNode, false);

#ifdef DISPLAY_DEBUG
        for (auto nodeIter=allNodes.begin(); nodeIter != allNodes.end(); nodeIter++)
            fprintf (stderr, "Evaluate Node: %s\n",(msra::strfun::utf8 ((*nodeIter)->NodeName())).c_str());
#endif

        // TODO: change this to a time stamp to make it consistent with PAR mode
        for (auto & recInfo : m_recurrentInfo)
            recInfo.m_completedEvaluate = false;

        // TODO: remove m_recurrentNodesRedundantCopy if we can
        for (const auto & recInfo : m_recurrentInfo)
            if (recInfo.m_recurrentNodesRedundantCopy != recInfo.m_recurrentNodes)
                LogicError("Evaluate: mismatch of m_recurrentNodesRedundantCopy for node %ls %ls", recInfo.m_sourceNode->NodeName().c_str(), recInfo.m_sourceNode->OperationName().c_str());

        // traverse all nodes in the pre-determined evaluation order
        for (auto & node : allNodes)
        {
            FrameRange frameRange(node->GetMBLayout());

            // --- if this node is part of a recurrence, evaluate all nodes that participate in this loop

            RecurrentInfo * recInfo = FindInRecurrentLoops(node);   // check if this node participates in a recurrent loop

            if (recInfo && IsFuncValueOlderThanInputs(recInfo->m_recurrentNodes) && !recInfo->m_completedEvaluate)
            {
#if 1
                recInfo->UpdateFunctionMBSize();
                recInfo->OnEvaluateBeginIteration();
                recInfo->EvaluateThisNode(frameRange);
                recInfo->OnEvaluateEndIteration();
#else
                // node participates in a recurrent loop: process the loop frame by frame
                const auto & recurrentNodes = recInfo->m_recurrentNodes;

                // get layout associated with this loop
                auto pMBLayout = recurrentNodes[0]->GetMBLayout();

                // tell all that loop is about to commence
                for (auto & node2 : recurrentNodes)
                {
                    if (!pMBLayout || node2->GetMBLayout() != pMBLayout)  // take the opportunity to check that layout is shared by all nodes in the loop
                        LogicError("Evaluate: all nodes inside a recurrent loop must have a layout that is identical; mismatch found for nodes '%ls' vs. '%ls'",
                                   node2->NodeName().c_str(), recurrentNodes[0]->NodeName().c_str());
                    node2->UpdateFunctionMBSize(); // TODO: for sequence-to-sequence models we will need to be able to grow this step by step since size is unknown upfront
                    node2->OnEvaluateBeginIteration();
                }

                //since we share memory we need to resize function value matrices correctly
                for (auto & node2 : recurrentNodes)
                {
                    //node2->UpdateFunctionMBSize();
                    node2->Validate(true);
                }

                // for every time step run through all nodes in this particular loop (treat the loop like a little ComputationNetwork)
                FrameRangeIteration range(pMBLayout, recInfo->m_steppingDirection);
                for (auto t = range.begin(); t != range.end(); t++)
                {
                    for (auto & node2 : recurrentNodes)
                    {
                        node2->EvaluateThisNode(t);
                        if (IsNodeReqMultiSeqHandling(node2))
                            node2->MaskMissingValuesColumnsToZero(t);
                        node2->UpdateEvalTimeStamp();
                    }
                } 

                // tell all that loop is done  --e.g. PastValueNode will capture its state for BPTT processing
                for (auto & node2 : recurrentNodes)
                    node2->OnEvaluateEndIteration();
#endif
                recInfo->m_completedEvaluate = true;
            }

            // --- not recurrent: do the whole batch (unless it's already done, e.g. because the node participated in a recurren ttloop)

            else if (!recInfo && node->IsFuncValueOlderThanInputs())
            {
#ifdef DISPLAY_DEBUG
                fprintf (stderr, "Evaluate Node: %s\n",(msra::strfun::utf8 (node->NodeName())).c_str());
#endif
#if DUMPOUTPUT
                fprintf(stderr,"Forward_%ls\n",node->NodeName().c_str());
#endif
                // evaluate the node for all frames concurrently (map)
                // we manage time stamp here so that derived classes don't need to worry about it
                node->UpdateFunctionMBSize();
                if (!node->IsLeaf() && !node->RequiresPreCompute())
                    node->Validate(true);                   // BUGBUG: Validate() should not be called during evaluation. This is meant to update m_functionValues' size in case of sharing.
                node->OnEvaluateBeginIteration();
                //fprintf(stderr, "EvaluateThisNode %d %ls %ls\n", -1, node->NodeName().c_str(), node->OperationName().c_str());
                node->EvaluateThisNode(frameRange);
                if (IsNodeReqMultiSeqHandling(node))
                    node->MaskMissingValuesColumnsToZero(frameRange);
                node->OnEvaluateEndIteration();
                node->UpdateEvalTimeStamp();
            }
#ifdef _DEBUG
            else
                node->OnEvaluateEndIteration();  // HACK: performs NaN check, but does nothing else
#endif
        }
    }

    // implementations of loop unrolling
    /*virtual*/ void ComputationNetwork::RecurrentInfo::UpdateFunctionMBSize() /*override*/
    {
        for (auto & node2 : m_recurrentNodes)
            node2->UpdateFunctionMBSize(); // TODO: for sequence-to-sequence models we will need to be able to grow this step by step since size is unknown upfront
    }

    /*virtual*/ void ComputationNetwork::RecurrentInfo::OnEvaluateBeginIteration() /*override*/
    {
        // get layout associated with this loop
        auto pMBLayout = m_recurrentNodes[0]->GetMBLayout();

        // tell all that loop is about to commence
        for (auto & node2 : m_recurrentNodes)
        {
            if (!pMBLayout || node2->GetMBLayout() != pMBLayout)  // take the opportunity to check that layout is shared by all nodes in the loop
                LogicError("Evaluate: all nodes inside a recurrent loop must have a layout that is identical; mismatch found for nodes '%ls' vs. '%ls'",
                            node2->NodeName().c_str(), m_recurrentNodes[0]->NodeName().c_str());
            node2->OnEvaluateBeginIteration();
        }

        // since we share memory we need to resize function value matrices correctly
        // TODO: No, Validate() should only run as a prep stage. This will go away once we separate dimension inference and actual resizing.
        for (auto & node2 : m_recurrentNodes)
            node2->Validate(true);
    }

    // evaluation of a RecurrentInfo FlowControlNode
    // This evaluates all nodes in this FlowControlNode in SEQ mode: process the loop frame by frame in a nested loop.
    // This is where the time axis changes.
    // TODO: Once we do nested loops, then the FrameRange argument to this will refer to the outer loop.
    /*virtual*/ void ComputationNetwork::RecurrentInfo::EvaluateThisNode(const FrameRange &) /*override*/
    {
        // get layout associated with this loop
        // All nodes share the same layout.
        auto pMBLayout = m_recurrentNodes[0]->GetMBLayout();

        // for every time step run through all nodes in this particular loop (treat the loop like a little ComputationNetwork)
        FrameRangeIteration range(pMBLayout, m_steppingDirection);
        for (auto t = range.begin(); t != range.end(); t++)
        {
            for (auto & node2 : m_recurrentNodes)
            {
                //fprintf(stderr, "EvaluateThisNode %d %ls %ls\n", (int)t.timeIdxInSeq, node2->NodeName().c_str(), node2->OperationName().c_str());
                node2->EvaluateThisNode(t);
                // TODO: this cannot be done since it is stored in the network now
                //if (IsNodeReqMultiSeqHandling(node2))
                //    node2->MaskMissingValuesColumnsToZero(t);
                node2->UpdateEvalTimeStamp();
            }
        } 
    }

    /*virtual*/ void ComputationNetwork::RecurrentInfo::OnEvaluateEndIteration() /*override*/
    {
        // tell all that loop is done  --e.g. PastValueNode will capture its state for BPTT processing
        for (auto & node2 : m_recurrentNodes)
            node2->OnEvaluateEndIteration();
    }

    // MAIN ENTRY POINT for evaluation followed by gradient computation (forward prop then back prop)
    // TODO: pass a set of nodes instead of only one?
    // TODO: remove Evaluate() from here, instead call it at call site, and in here merely check whether everything is computed already
    // BUGBUG: The decision to loop (SEQ execution) is made by parent, but some children can be executer PAR. It should be possible to detect this.
    template<class ElemType>
    void ComputationNetwork::ComputeGradient(const ComputationNodeBasePtr rootNode,         // training criterion to compute the gradients for
                                             bool bResetToOne,                              // true if reset the gradient of rootnode to 1.0  --This is the default.
                                             const Matrix<ElemType>* rootGradientInitValue, // if given then this is the starting gradient from the top
                                             bool bClearGradient,                           // if false then gradients are not cleared  --TODO: When does that happen?
                                             bool resetTimeStampAfterComputation)
    {
        // run forward pass first for criterion node
        // The actual call pattern is
        //  - Evaluate() for eval nodes
        //  - ComputeGradient() for the training criterion
        // I.e. we must call Evaluate() inside here as well, but it will typically only evaluate the training criterion bits because the eval nodes already require most of the network to be computed.
        Evaluate(rootNode);

        // TODO: comment what the purpose/condition of this is
        if (bClearGradient)
            ClearGradientForAllNodes(rootNode);

        // run backprop pass
        std::list<ComputationNodeBasePtr>& allNodes = GetGradientCalcOrder(rootNode);

        // TODO: do a runtime check for float vs. double. Also use the Is/AsPtr macros
        // The normal case is with the top root with a scalar gradient value of 1.0. This assumes a single and closure network. 
        // Allowing to not initialize to 1 allows network to be open to accept gradients from somewhere.
        // TODO: aren't these two mechanisms mutually exclusive?
        if (bResetToOne)
        {
            dynamic_pointer_cast<ComputationNode<ElemType>>(rootNode)->GradientValues().Resize(1, 1);   // TODO: make this a function of ComputationNode; but first need to get rid of Matrix<ElemType> here, or make it a local template parameter
            dynamic_pointer_cast<ComputationNode<ElemType>>(rootNode)->GradientValues().SetValue(1);    // TODO: is there not a single SetValue() call that also takes dimensions?
        }

        if (rootGradientInitValue != nullptr)   // user-specified gradient to start with
            dynamic_pointer_cast<ComputationNode<ElemType>>(rootNode)->GradientValues().SetValue(*rootGradientInitValue);

        // process nodes in pre-determined order
        for (auto & node : allNodes)
        {
#ifdef DISPLAY_DEBUG
            fprintf(stderr, "Compute Gradient For Node: %ls(%ls) Against Children\n", node->OperationName().c_str(), node->NodeName().c_str());
#endif
            // --- first, perform recurrent loops if this node participates in one

            RecurrentInfo * recInfo = FindInRecurrentLoops(node);
            if (recInfo)
            {
                if (!recInfo->m_completedGradient)
                {
#if 1
                    recInfo->OnComputeGradientBeginIteration();
                    recInfo->ComputeGradientForChildren(FrameRange(node->GetMBLayout()), true, true);
                    recInfo->OnComputeGradientEndIteration();
#else
                    const auto & recurrentNodes = recInfo->m_recurrentNodes;
                    for (auto & node2 : recurrentNodes)
                        node2->OnComputeGradientBeginIteration();
                    auto pMBLayout = recurrentNodes[0]->GetMBLayout();
                    FrameRangeIteration range(pMBLayout, recInfo->m_steppingDirection);
                    for (auto t = range.rbegin(); t != range.rend(); t++)   // note: reverse iteration
                    {
                        for (auto nodeIter2 = recurrentNodes.rbegin(); nodeIter2 != recurrentNodes.rend(); ++nodeIter2)
                        {
                            auto & node2 = *nodeIter2;
                            node2->VerifyNumParallelSequences(GetNumParallelSequences());
                            if (IsNodeReqMultiSeqHandling(node2))
                                node2->MaskMissingGradientColumnsToZero(t);
                            // TODO: exclude children that are not part of the recurrent loop, and do thise below, separately.
                            node2->ComputeGradientForChildren(t);
                        }
                    }
                    for (auto & node2 : recurrentNodes)
                        node2->OnComputeGradientEndIteration();
#endif
                    recInfo->m_completedGradient = true;
                }
            }

            // --- second, do whole-batch operation if not recurrent

            else
            {
                node->OnComputeGradientBeginIteration();
                if (IsNodeReqMultiSeqHandling(node))    // (TODO: This will go away.)
                {
                    // batch is done only for feed-forward nodes
                    if (node->IsPartOfLoop()) // (this test was moved out from MaskMissingGradientColumnsToZero(void), it is likely unnecessary)
                        LogicError("Evaluate: Applying whole-MB operation to node that participates in a loop. This is likely wrong.");
                    node->MaskMissingGradientColumnsToZero(FrameRange(node->GetMBLayout()));
                }
                node->ComputeGradientForChildren(FrameRange(node->GetMBLayout()), true, true);
                node->OnComputeGradientEndIteration();
            }
        }

        //since we now allow sharing of the matrix for function value and gradient value. the function values are now destroyed
        //after gradient computation and need to be recomputed. This is indicated by the timestamp updated using this function
        //resetTimeStampAfterComputation is by default false because ComputeGradient in normal case is followed by new batch of input
        if (resetTimeStampAfterComputation)
            ResetEvalTimeStamp();
    }

    // called before first iteration step of ComputeGradient()
    /*virtual*/ void ComputationNetwork::RecurrentInfo::OnComputeGradientBeginIteration() /*override*/
    {
        for (auto & node2 : m_recurrentNodes)
            node2->OnComputeGradientBeginIteration();
    }
    /*virtual*/ void ComputationNetwork::RecurrentInfo::ComputeGradientForChildren(const FrameRange &, bool childrenInThisLoop, bool childrenInOuterLoop) /*override*/
    {
        childrenInThisLoop, childrenInOuterLoop;    // TODO: think through what these mean when coming from PAR mode
        const auto & recurrentNodes = m_recurrentNodes;       // BUGBUG: -ForForward?? Does this mean we can remove non-ForForward?
        auto pMBLayout = recurrentNodes[0]->GetMBLayout();
        FrameRangeIteration range(pMBLayout, m_steppingDirection);
        for (auto t = range.rbegin(); t != range.rend(); t++)   // note: reverse iteration
        {
            for (auto nodeIter2 = recurrentNodes.rbegin(); nodeIter2 != recurrentNodes.rend(); ++nodeIter2)
            {
                auto & node2 = *nodeIter2;
                // BUGBUG: The following can no longer be done after this code was moved into RecurrentInfo
                //node2->VerifyNumParallelSequences(GetNumParallelSequences());
                //if (IsNodeReqMultiSeqHandling(node2))
                //    node2->MaskMissingGradientColumnsToZero(t);
                // TODO: exclude children that are not part of the recurrent loop, and do thise below, separately.
#define OPT_OUTER_GRADIENT  // if true then we compute the gradient outside of the loop where it is possible
#ifdef OPT_OUTER_GRADIENT
                node2->ComputeGradientForChildren(t, true/*childrenInThisLoop*/, false/*childrenInOuterLoop*/);
#else
                node2->ComputeGradientForChildren(t, true/*childrenInThisLoop*/, true/*childrenInOuterLoop*/);
#endif
            }
        }
    }
    // called after last iteration step of ComputeGradient()
    /*virtual*/ void ComputationNetwork::RecurrentInfo::OnComputeGradientEndIteration() /*override*/
    {
#ifdef OPT_OUTER_GRADIENT
        for (auto nodeIter2 = m_recurrentNodes.rbegin(); nodeIter2 != m_recurrentNodes.rend(); ++nodeIter2)
        {
            auto & node2 = *nodeIter2;
            // BUGBUG: The following can no longer be done after this code was moved into RecurrentInfo
            //node2->VerifyNumParallelSequences(GetNumParallelSequences());
            //if (IsNodeReqMultiSeqHandling(node2))
            //    node2->MaskMissingGradientColumnsToZero(t);
            // TODO: exclude children that are not part of the recurrent loop, and do thise below, separately.
            node2->ComputeGradientForChildren(FrameRange(m_recurrentNodes[0]->GetMBLayout()), false/*childrenInThisLoop*/, true/*childrenInOuterLoop*/);
        }
#endif
        for (auto & node2 : m_recurrentNodes)
            node2->OnComputeGradientEndIteration();
    }

    template void ComputationNetwork::ComputeGradient<float>(const ComputationNodeBasePtr rootNode, bool bResetToOne, const Matrix<float>* rootGradientInitValue, bool bClearGradient, bool resetTimeStampAfterComputation);
    template void ComputationNetwork::ComputeGradient<double>(const ComputationNodeBasePtr rootNode, bool bResetToOne, const Matrix<double>* rootGradientInitValue, bool bClearGradient, bool resetTimeStampAfterComputation);

    // find if node is part of a recurrent loop; and return the loop id
    // If found then return a pointer to the list of nodes of this loop.
    // TODO: This should just return &m_recurrentInfo of the matching loop, or nullptr if no match. If needed, m_recurrentInfo knows its loop id.
    ComputationNetwork::RecurrentInfo * ComputationNetwork::FindInRecurrentLoops(const ComputationNodeBasePtr& node)
    {
        // look in all recurrent loops of the network
        // TODO: Check for IsPartOfLoop(). Also why not store the loop id in the node for direct lookup?
        for (auto & iter : m_recurrentInfo)
            if (std::find(iter.m_recurrentNodesRedundantCopy.begin(), iter.m_recurrentNodesRedundantCopy.end(), node) != iter.m_recurrentNodesRedundantCopy.end())
                return &iter;
        return nullptr;  // not part of a recurrent loop
    }

    bool ComputationNetwork::IsFuncValueOlderThanInputs(const vector<ComputationNodeBasePtr>& recurrentNodes)
    {
        for (auto ptr = recurrentNodes.begin(); ptr != recurrentNodes.end(); ptr++)
        {
            if ((*ptr)->IsFuncValueOlderThanInputs() && 
                (*ptr)->OperationName() != OperationNameOf(PastValueNode) &&
                (*ptr)->OperationName() != OperationNameOf(FutureValueNode))
            {
                return true;
            }
        }
        return false;
    }

    void ComputationNetwork::ResetEvalTimeStamp()
    {
        for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
            nodeIter->second->ResetEvalTimeStamp();
    }

    /*static*/void ComputationNetwork::UpdateEvalTimeStamps(const vector<ComputationNodeBasePtr> & nodes)
    {
        for (size_t i = 0; i<nodes.size(); i++)
            nodes[i]->UpdateEvalTimeStamp();
    }

    // for debugging
    void ComputationNetwork::PrintComputationTree(const ComputationNodeBasePtr& rootNode,
                                                  const bool forwardCompute,
                                                  const bool printMatrices)
    {
        std::list<ComputationNodeBasePtr> nodes;
        if (forwardCompute)
        {
            fprintf(stderr, "\n\nPrinting Forward Computation Node Order ... \n");
            nodes = GetEvalOrder(rootNode, false);
        }
        else
        {
            fprintf(stderr, "\n\nPrinting Gradient Computation Node Order ... \n");
            nodes = GetGradientCalcOrder(rootNode);
        }

        if (nodes.size() == 0)
        {
            fprintf(stderr, "\n$$$$ EMPTY !!!!!\n");
            return;
        }

        for (auto nodeIter = nodes.begin(); nodeIter != nodes.end(); nodeIter++)
        {
            ComputationNodeBasePtr node = (*nodeIter);
            node->PrintSelf(printMatrices);
        }
    }

    // -----------------------------------------------------------------------
    // validation
    // -----------------------------------------------------------------------

    // ValidateNetwork() - Validate the entire network
    // This calls ValidateNetowrk(Node) for all output nodes.
    // This is used after loading or for dumping the network.
    void ComputationNetwork::ValidateNetwork(bool allowFragment, const bool bAllowNoCriterion)
    {
        // currently only validates nodes, we should validate everything we can
        if (FeatureNodes().size() == 0 && !allowFragment)
            RuntimeError("No Feature nodes specified");

        // TODO: allocation does not belong here. This is called e.g. after loading. Memory should be allocated only when actually evaluating.
        AllocateAllEvalMatrices(EvaluationNodes(), OutputNodes(), FinalCriterionNodes());
        // first give criteria nodes as root node
        if (FinalCriterionNodes().size() > 0)
        {
            for (ComputationNodeBasePtr & node : FinalCriterionNodes())
            {
                if (!allowFragment)
                    FormRecurrentLoops(node);
#ifdef _DEBUG
                PrintComputationTree(node, false);
#endif
                //SetActualMiniBatchSizeFromFeatures();
                ValidateSubNetwork(node);
            }
        }
        else if (bAllowNoCriterion == true)
        {
            // do nothing
        }
        else if (!allowFragment)
            RuntimeError("No Criterion nodes specified");

        // now output nodes
        if (OutputNodes().size() > 0)
        {
            for (ComputationNodeBasePtr node : OutputNodes())
            {
                if (!allowFragment)
                    FormRecurrentLoops(node);
                ValidateSubNetwork(node);
            }
        }
        else if (!allowFragment)
            RuntimeError("No Output nodes specified");

        // now evaluation nodes
        if (EvaluationNodes().size() > 0)
        {
            for (ComputationNodeBasePtr node : EvaluationNodes())
            {
                if (!allowFragment)
                    FormRecurrentLoops(node);
                ValidateSubNetwork(node);
            }
        }
    }

    // validate sub-network needed to evalute a specific output node
    // This calls Validate() on every node in evaluation order (allowing to propagate things forwards through the net).
    // This is called lazily but once only per node until next ClearCache().
    // This also sets up MBLayout links.
    // TODO: I can't see a clear pattern when ClearCache() is called. E.g. at the start of each epoch? Or never in normal operation (init only at construction)?
    // Note: under some circumstances, one must call FormRecurrentNodes() on this node before calling this. TODO: Not clear which ones.
    // TODO: ^^ is this really needed? Can we just call it inside?
    void ComputationNetwork::ValidateSubNetwork(const ComputationNodeBasePtr& rootNode)
    {
        // set up MBLayout links of inputs (all others get propagated upwards through Validate())
        // TODO: Once we support mismatching layouts, this will be more involved. For now, everything shares the one layout that the Network knows about.
        for (auto node : InputNodes(rootNode))
        {
            node->LinkToMBLayout(m_pMBLayout);
            // handle the special case of being validated before reading a minibatch
            // In that case, the layout is empty. We set up a dummy layout to match the first InputValue.
            // TODO: This is a stop-gap. We need a better-controlled way of when what gets validated.
            if (m_pMBLayout->GetNumCols() == 0)
                m_pMBLayout->Init(1, node->GetNumCols(), false);
        }

        // we call all nodes' Validate() in order to validate, that is, set up MBLayout and FunctionValues dimension
        // A problem is that recurrent loops may require partial validation.
        // Nodes validated on partial input (i.e. some children not yet validated) will be revisited.
        const auto & nodes = GetEvalOrder(rootNode, false);

        for (auto & node : nodes)
        {
            node->m_visited = false;
            node->m_needsGradient = node->IsParameterUpdateRequired();  // these get propagated upwards in the following
        }

        // loop and validate until we are done
        // steps:
        //  - validate (not final)          // not final means no dimension checks
        //    Keep going through the list until all nodes have been validated and all inputs have been validated as well.
        //  - validate (final)              // final means consistency checks
        //    Fail if any change during this stage.
        size_t pass = 0;
        size_t toValidate = nodes.size();
        while (toValidate > 0)
        {
            pass++;
            fprintf(stderr, "\n\nValidating for node %ls. %d nodes to process in pass %d.\n", rootNode->NodeName().c_str(), (int)toValidate, (int)pass);
            ValidateNodes(nodes, false/*isFinalValidationPass*/, toValidate);
        }
        fprintf(stderr, "\n\nValidating for node %ls, final verification.\n", rootNode->NodeName().c_str());
        ValidateNodes(nodes, true/*isFinalValidationPass*/, toValidate);
        if (toValidate != 0)
            LogicError("ValidateSubNetwork: ValidateNodes(true) unexpectedly returned with work left to do.");

        for (auto & node : nodes)
        {
#if 0       // not possible once we have inconsistent layouts
            // verify that the contract with MB layout was obeyed by Validate()
            if (node->GetMBLayout() && node->GetMBLayout()->GetNumCols() != node->GetNumCols())
            {
                fprintf(stderr, "\n%ls %ls operation's Validate() function set function values width (%d) inconsistent with MB layout width (T=%d x S=%d)\n",
                        node->NodeName().c_str(), node->OperationName().c_str(), (int)node->GetNumCols(), (int)node->GetNumTimeSteps(), (int)node->GetNumParallelSequences());
                LogicError("%ls %ls operation's Validate() function set function values width (%d) inconsistent with MB layout width (T=%d x S=%d)",
                           node->NodeName().c_str(), node->OperationName().c_str(), (int)node->GetNumCols(), (int)node->GetNumTimeSteps(), (int)node->GetNumParallelSequences());
            }
#endif
            // nodes must output non-zero dimensional data, otherwise assume user error
            if (node->GetNumRows() == 0 && (node->GetMBLayout() || node->GetNumCols() == 0))
                RuntimeError("%ls operation has 0 elements", node->NodeName().c_str());
        }
        fprintf(stderr, "\n\n");

        // logging the non-default-layout nodes
        vector<ComputationNodeBasePtr> nonDefaultNodes;
        for (auto node : nodes)
        {
            if (!(node->GetMBLayout() == m_pMBLayout))
                nonDefaultNodes.push_back(node);
        }
        if (!nonDefaultNodes.empty())
        {
            fprintf(stderr, "%d out of %d nodes do not share the minibatch layout with the input data.\n\n", (int)nonDefaultNodes.size(), (int)nodes.size());
            //for (auto node : nonDefaultNodes)
            //    fprintf(stderr, "    %ls\n", node->NodeName().c_str());
            //fprintf(stderr, "\n\n");
        }
    }

    void ComputationNetwork::ValidateNodes(list<ComputationNodeBasePtr> nodes, bool isFinalValidationPass, size_t & todo)
    {
        todo = 0;           // returns how many nodes are to be redone
        for (auto & node : nodes)
        {
            const auto & children = node->GetChildren();
            const bool isLeaf = node->IsLeaf();
            // only validate a node if it has at least one child
            bool hasVisitedChild = false;
            bool allChildrenVisited = true;
            for (auto & child : children)
            {
                hasVisitedChild |= child->m_visited;    // if not a single visited child then no point in validating
                allChildrenVisited &= child->m_visited;
            }
            // if there is not at least one visited child
            bool valid = false;
            if (hasVisitedChild || isLeaf)
            {
                // got at least one child: it makes sense to call Validate()
                // keep state
                MBLayoutPtr oldMBLayoutPtr = node->GetMBLayout();
                auto dim = node->GetDims();
                vector<pair<size_t, size_t>> childDims;
                for (auto & child : children)
                    childDims.push_back(child->GetDims());
                auto imageLayouts = node->GetImageLayouts();
                // We do call validate(final) as many times as needed, since stuff may have changed underneath.
                node->PrintSelfBeforeValidation();
                node->Validate(isFinalValidationPass/*final*/);      // all nodes have been visited: do verification instead of just inference
                fprintf(stderr, " -> [%lu, %s%lu]", node->GetNumRows(), node->HasMBLayout() ? "MBSize " : "", node->GetNumCols());
                node->m_visited = true;
                // also take the opportunity to propagate m_needsGradient
                auto needsGradient = node->m_needsGradient;
                for (auto & child : children)       // TODO: do we need a check that this is stable if isFinalValidationPass?
                    node->m_needsGradient |= child->m_needsGradient;
                // check state --node will be valid if all nodes have been visited and node has not been updated
                bool unchanged = true;
                unchanged &= (oldMBLayoutPtr == node->GetMBLayout());
                unchanged &= (dim == node->GetDims());
                vector<pair<size_t, size_t>> newChildDims;
                for (auto & child : children)
                    newChildDims.push_back(child->GetDims());
                unchanged &= (childDims == newChildDims);
                unchanged &= (imageLayouts == node->GetImageLayouts());
                unchanged &= (needsGradient == node->m_needsGradient);
                if (isFinalValidationPass && !unchanged)
                    LogicError("ValidateSubNetwork: %ls %ls operation changed during final validation.", node->NodeName().c_str(), node->OperationName().c_str());
                if (isFinalValidationPass && !allChildrenVisited)
                    LogicError("ValidateSubNetwork: %ls %ls operation in final validation although not all children were visited?", node->NodeName().c_str(), node->OperationName().c_str());
                // if all children valid then 
                valid = (allChildrenVisited && unchanged) || isLeaf;
            }
            // count those that we need to redo
            if (!valid)
                todo++;
        }
    }

    // prepare to compute with the subnetwork that this rootNode depends on, including
    //  - auto-detecting recurrent loops
    //  - collect input and learnable nodes
    //  - calling Validate() on all nodes lazily, which sizes all matrices (column dimensions get updated to MB size)
    // Done lazily, called for every minibatch's invocation of EvaluateNode(), but memoizing which nodes were done already.
    // BUGBUG? Lazy triggers on the root node. I.e. for two different root nodes (training, eval), it validates twice.
    void ComputationNetwork::BuildAndValidateSubNetwork(const ComputationNodeBasePtr rootNode)
    {
        const auto inserted = m_built.insert(rootNode).second;  // remember we built it
        if (!inserted)
            return;                                             // already done

        // detect recurrent loops for this root node
        // TODO: not nice--why not always call this in ValidateSubNetwork() only?
        FormRecurrentLoops(rootNode);

        // for the m_inputs and m_learnableParameters sets for this rootNode
        CollectInputAndLearnableParameters(rootNode);

        // validate the rootNode and all nodes it depends on, in evaluation order
        ValidateSubNetwork(rootNode);

        // (gone: now done more directly without state in ComputationNode)
        //SetRequestNodesMultiSeqHandling();
    }

    bool ComputationNetwork::BuiltAndValidatedSubNetwork(const ComputationNodeBasePtr & rootNode)
    {
        return m_built.find(rootNode) != m_built.end();
    }

    // -----------------------------------------------------------------------
    // memory allocation
    // -----------------------------------------------------------------------

    //this function will need to be called before actual validation and execution to 
    //predetermine how to share matrices to reduce memory usage.
    //TODO: find a simple topological order and allocateEvalMatrices on that order directly
    //without passing in eval, out, and train nodes.
    void ComputationNetwork::AllocateAllEvalMatrices(std::vector<ComputationNodeBasePtr>& evalRootNodes,
                                                     std::vector<ComputationNodeBasePtr>& outValueRootNodes,
                                                     std::vector<ComputationNodeBasePtr>& trainRootNodes)
    {
        //allocate memory for forward computation
        fprintf(stderr, "\n\nAllocating matrices for forward propagation.\n");
        for (int i = 0; i < evalRootNodes.size(); i++)
            AllocateEvalMatrices(evalRootNodes[i]);
        for (int i = 0; i < outValueRootNodes.size(); i++)
            AllocateEvalMatrices(outValueRootNodes[i]);
        for (int i = 0; i < trainRootNodes.size(); i++)
            AllocateEvalMatrices(trainRootNodes[i]);

    }

    void ComputationNetwork::AllocateEvalMatrices(ComputationNodeBasePtr rootNode)
    {
        FormRecurrentLoops(rootNode);

        std::list<ComputationNodeBasePtr>& allNodes = GetEvalOrder(rootNode, false);

        //determine parent size
        std::map<ComputationNodeBasePtr, int> parentCount;
        for (auto &n : allNodes)
        {
            for (int i = 0; i < n->ChildrenSize(); i++)
            {
                ComputationNodeBasePtr pNode = n->GetChildren()[i];
                parentCount[pNode]++;
            }
        }

        for (int i = 0; i < m_recurrentInfo.size(); i++)
            m_recurrentInfo[i].m_completedEvaluate = false;

        for (auto &nodeIter : allNodes)
        {
            if (nodeIter->IsPartOfLoop())
            {
                RecurrentInfo* recInfo = FindInRecurrentLoops(nodeIter);
                assert(recInfo != nullptr);
                if (recInfo->m_completedEvaluate == false)
                {
                    const auto & recurrentNodes = recInfo->m_recurrentNodes;
                    for (auto &nodeLoopIter : recurrentNodes)
                    {
                        nodeLoopIter->RequestMatricesBeforeEval(m_matrixPool);
                    }

                    recInfo->m_completedEvaluate = true;

                    for (auto &nodeLoopIter : recurrentNodes)
                    {
                        ReleaseMatricesAfterEvalForChildren(nodeLoopIter, parentCount);
                    }
                }
            }
            else
            {
                nodeIter->RequestMatricesBeforeEval(m_matrixPool);
                //we only release matrices for the children since the root node's informatioin will be used and should not be shared
                //with others
                ReleaseMatricesAfterEvalForChildren(nodeIter, parentCount);
            }
        }
    }

    void ComputationNetwork::ReleaseMatricesAfterEvalForChildren(ComputationNodeBasePtr n, std::map<ComputationNodeBasePtr, int>& parentCount)
    {
        for (int i = 0; i < n->ChildrenSize(); i++)
        {
            ComputationNodeBasePtr pNode = n->GetChildren()[i];
            parentCount[pNode]--;
            if (parentCount[pNode] == 0)
                pNode->ReleaseMatricesAfterEval(m_matrixPool);
        }
    }

    void ComputationNetwork::AllocateGradientMatrices(ComputationNodeBasePtr rootNode)
    {
        FormRecurrentLoops(rootNode);

        //PopulateParents(rootNode);
        std::list<ComputationNodeBasePtr>& allNodes = GetGradientCalcOrder(rootNode);

        //determine children size
        //std::map<ComputationNodeBasePtr, int> childrenCount;
        //for (auto &nodeIter : allNodes)
        //{
        //    childrenCount[nodeIter] = nodeIter->ChildrenSize();
        //}

        //now, simulate the gradient computation order to determine how to allocate matrices
        for (int i = 0; i < m_recurrentInfo.size(); i++)
            m_recurrentInfo[i].m_completedGradient = false;

        //we need to call it here since we always compute gradients for children and root node is not children of other node
        rootNode->RequestMatricesBeforeGradientComp(m_matrixPool);

        for (auto &n : allNodes)
        {
            if (n->IsPartOfLoop())
            {
                std::vector<ComputationNodeBasePtr> recurrentNodes;
                RecurrentInfo * recInfo = FindInRecurrentLoops(n);
                if (recInfo && recInfo->m_completedGradient == false)
                {
                    const auto & recurrentNodes = recInfo->m_recurrentNodes;
                    //loops are computed sample by sample so we have to allocate them all 
                    for (auto nodeIter = recurrentNodes.rbegin(); nodeIter != recurrentNodes.rend(); ++nodeIter)
                    {
                        AllocateGradientMatricesForChildren(*nodeIter);
                    }
                    recInfo->m_completedGradient = true;
                    for (auto nodeIter = recurrentNodes.rbegin(); nodeIter != recurrentNodes.rend(); ++nodeIter)
                    {
                        if ((*nodeIter)->NeedGradient())
                        {
                            (*nodeIter)->ReleaseMatricesAfterGradientComp(m_matrixPool);
                        }
                    }
                }
            }
            else
            {
                AllocateGradientMatricesForChildren(n);
                if ((n != rootNode) && n->NeedGradient())  //root node's informatioin will be used and should not be shared with others, also it's small (1x1)
                    n->ReleaseMatricesAfterGradientComp(m_matrixPool);
            }
        }
    }

    //void ReleaseMatricesAfterGradientCompForParents(ComputationNodeBasePtr n, std::map<ComputationNodeBasePtr, int>& childrenCount)
    //{
    //    for (int i = 0; i < n->ParentSize(); i++)
    //    {
    //        ComputationNodeBasePtr pNode = n->Parent(i);
    //        childrenCount[pNode] --;
    //        if (childrenCount[pNode] == 0)
    //            pNode->ReleaseMatricesAfterGradientComp(m_matrixPool);
    //    }
    //}
  

    void ComputationNetwork::AllocateGradientMatricesForChildren(ComputationNodeBasePtr parentNode)
    {
        std::vector<ComputationNodeBasePtr> children = parentNode->GetChildren();
        for (int i = 0; i < children.size(); i++)
        {
            if (children[i]->NeedGradient())
                children[i]->RequestMatricesBeforeGradientComp(m_matrixPool);
        }
    }

}}}