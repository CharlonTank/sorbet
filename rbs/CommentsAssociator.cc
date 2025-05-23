#include "rbs/CommentsAssociator.h"

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "common/typecase.h"
#include "core/errors/rewriter.h"
#include "parser/helper.h"
#include "rbs/SignatureTranslator.h"

using namespace std;

namespace sorbet::rbs {

const string_view CommentsAssociator::RBS_PREFIX = "#:";
const string_view CommentsAssociator::ANNOTATION_PREFIX = "# @";
const string_view CommentsAssociator::MULTILINE_RBS_PREFIX = "#|";

namespace {
bool isVisibilitySend(const parser::Send *send) {
    return send->receiver == nullptr && send->args.size() == 1 &&
           (parser::isa_node<parser::DefMethod>(send->args[0].get()) ||
            parser::isa_node<parser::DefS>(send->args[0].get())) &&
           (send->method == core::Names::private_() || send->method == core::Names::protected_() ||
            send->method == core::Names::public_() || send->method == core::Names::privateClassMethod() ||
            send->method == core::Names::publicClassMethod() || send->method == core::Names::packagePrivate() ||
            send->method == core::Names::packagePrivateClassMethod());
}

bool isAttrAccessorSend(const parser::Send *send) {
    return (send->receiver == nullptr || parser::isa_node<parser::Self>(send->receiver.get())) &&
           (send->method == core::Names::attrReader() || send->method == core::Names::attrWriter() ||
            send->method == core::Names::attrAccessor());
}
}; // namespace

void CommentsAssociator::consumeCommentsUntilLine(int line) {
    auto it = commentByLine.begin();
    while (it != commentByLine.end()) {
        if (it->first < line) {
            if (absl::StartsWith(it->second.string, RBS_PREFIX) ||
                absl::StartsWith(it->second.string, MULTILINE_RBS_PREFIX)) {
                if (auto e = ctx.beginIndexerError(it->second.loc, core::errors::Rewriter::RBSUnusedComment)) {
                    e.setHeader("Unused RBS signature comment. No method definition found after it");
                }
            }
            ++it;
        } else {
            break;
        }
    }
    commentByLine.erase(commentByLine.begin(), it);
}

void CommentsAssociator::associateCommentsToNode(parser::Node *node) {
    auto nodeStartLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.beginPos()).line;

    vector<CommentNode> comments;

    for (auto it = commentByLine.begin(); it != commentByLine.end();) {
        auto commentLine = it->first;

        if (commentLine >= nodeStartLine) {
            break;
        }

        if (absl::StartsWith(it->second.string, RBS_PREFIX) ||
            absl::StartsWith(it->second.string, MULTILINE_RBS_PREFIX) ||
            absl::StartsWith(it->second.string, ANNOTATION_PREFIX)) {
            comments.emplace_back(it->second);
            it = commentByLine.erase(it);
            continue;
        }

        it++;
    }

    commentsByNode[node] = move(comments);
}

void CommentsAssociator::walkNodes(parser::Node *node) {
    if (node == nullptr) {
        return;
    }

    typecase(
        node,

        // Nodes that need to consume preceding comments as well as comments until the end of the node

        [&](parser::Kwbegin *kwbegin) {
            auto beginLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.beginPos()).line;
            consumeCommentsUntilLine(beginLine);

            for (auto &stmt : kwbegin->stmts) {
                walkNodes(stmt.get());
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Class *cls) {
            associateCommentsToNode(node);
            auto beginLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.beginPos()).line;
            consumeCommentsUntilLine(beginLine);

            if (auto body = cls->body.get()) {
                walkNodes(body);
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::SClass *sclass) {
            associateCommentsToNode(node);
            auto beginLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.beginPos()).line;
            consumeCommentsUntilLine(beginLine);

            if (auto body = sclass->body.get()) {
                walkNodes(body);
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Module *mod) {
            associateCommentsToNode(node);
            auto beginLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.beginPos()).line;
            consumeCommentsUntilLine(beginLine);

            if (auto body = mod->body.get()) {
                walkNodes(body);
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },

        // Nodes that need to consume comments until the end of the node

        [&](parser::Begin *begin) {
            for (auto &stmt : begin->stmts) {
                walkNodes(stmt.get());
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::DefMethod *def) {
            associateCommentsToNode(node);
            walkNodes(def->body.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::DefS *def) {
            associateCommentsToNode(node);
            walkNodes(def->body.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Send *send) {
            if (isVisibilitySend(send)) {
                associateCommentsToNode(send->args[0].get());
                auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
                consumeCommentsUntilLine(endLine);
            } else if (isAttrAccessorSend(send)) {
                associateCommentsToNode(send);
                auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
                consumeCommentsUntilLine(endLine);
            }
        },
        [&](parser::Assign *assign) {
            if (auto rhs = assign->rhs.get()) {
                walkNodes(rhs);
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::AndAsgn *andAsgn) {
            if (auto rhs = andAsgn->right.get()) {
                walkNodes(rhs);
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::OrAsgn *orAsgn) {
            if (auto rhs = orAsgn->right.get()) {
                walkNodes(rhs);
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::OpAsgn *opAsgn) {
            if (auto rhs = opAsgn->right.get()) {
                walkNodes(rhs);
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Masgn *masgn) {
            walkNodes(masgn->rhs.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Block *block) {
            auto beginLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.beginPos()).line;
            consumeCommentsUntilLine(beginLine);

            walkNodes(block->body.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::For *forNode) {
            auto beginLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.beginPos()).line;
            consumeCommentsUntilLine(beginLine);

            walkNodes(forNode->body.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Array *array) {
            for (auto &elem : array->elts) {
                walkNodes(elem.get());
            }
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Rescue *rescue) {
            walkNodes(rescue->body.get());
            for (auto &rescued : rescue->rescue) {
                walkNodes(rescued.get());
            }
            walkNodes(rescue->else_.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Ensure *ensure) {
            walkNodes(ensure->body.get());
            walkNodes(ensure->ensure.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::If *if_) {
            walkNodes(if_->then_.get());
            walkNodes(if_->else_.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Resbody *resbody) {
            walkNodes(resbody->body.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Case *case_) {
            for (auto &when : case_->whens) {
                walkNodes(when.get());
            }
            walkNodes(case_->else_.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::When *when) {
            walkNodes(when->body.get());
            auto endLine = core::Loc::pos2Detail(ctx.file.data(ctx), node->loc.endPos()).line;
            consumeCommentsUntilLine(endLine);
        },
        [&](parser::Node *other) {
            // Do nothing
        });
}

std::map<parser::Node *, vector<CommentNode>> CommentsAssociator::run(unique_ptr<parser::Node> &node) {
    walkNodes(node.get());
    return move(commentsByNode);
};

CommentsAssociator::CommentsAssociator(core::MutableContext ctx, vector<core::LocOffsets> commentLocations)
    : ctx(ctx), commentLocations(commentLocations), commentByLine() {
    for (auto &loc : commentLocations) {
        auto comment_string = ctx.file.data(ctx).source().substr(loc.beginPos(), loc.endPos() - loc.beginPos());
        auto start32 = static_cast<uint32_t>(loc.beginPos());
        auto end32 = static_cast<uint32_t>(loc.endPos());
        auto comment = CommentNode{core::LocOffsets{start32, end32}, comment_string};

        auto line = core::Loc::pos2Detail(ctx.file.data(ctx), start32).line;
        commentByLine[line] = comment;
    }
}

} // namespace sorbet::rbs
