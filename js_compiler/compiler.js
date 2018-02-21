/*************************************
CONVERT B9 PORCELAIN TO BINARY MODULE 
**************************************/

// syntax is <in> <out>

var esprima = require('esprima');
var fs = require('fs');

function outputUInt32(out, value) {

	var buf = Buffer.allocUnsafe(4);
	buf.writeUInt32LE(value, 0);
	fs.writeSync(out, buf);
}

function outputString(out, string) {
	outputUInt32(out, string.length);
	fs.writeSync(out, string);
}

function primitiveCode(primitiveName) {
	var primitives = { "print_string": 0, "print_number": 1 };
	return primitives[primitiveName];
}

function operatorCode(operator) {
	switch (operator) {
		case "END_SECTION":
			return 0;
		case "FUNCTION_CALL":
			return 1;
		case "FUNCTION_RETURN":
			return 2;
		case "PRIMITIVE_CALL":
			return 3;
		case "JMP":
			return 4;
		case "DUPLICATE":
			return 5;
		case "DROP":
			return 6;
		case "PUSH_FROM_VAR":
			return 7;
		case "POP_INTO_VAR":
			return 8;
		case "INT_ADD":
			return 9;
		case "INT_SUB":
			return 10;
		case "INT_PUSH_CONSTANT":
			return 13;
		case "INT_NOT":
			return 14;
		case "INT_JMP_EQ":
			return 15;
		case "INT_JMP_NEQ":
			return 16;
		case "INT_JMP_GT":
			return 17;
		case "INT_JMP_GE":
			return 18;
		case "INT_JMP_LT":
			return 19;
		case "INT_JMP_LE":
			return 20;
		case "STR_PUSH_CONSTANT":
			return 21;
		case "STR_JMP_EQ":
			return 22;
		case "STR_JMP_NEQ":
			return 23;
		default:
			return -1;
	}
};

/// operator - name of operator (NOT CODE)
/// operand - numeric operand, if applicable.
var Instruction = function (operator, operand) {

	this.output = function (out) {
		var encoded = (operatorCode(this.operator) << 24);

		if (this.operand) {
			encoded |= this.operand & 0x00FFFFFF;
		}

		encoded &= 0xFFFFFFFF;

		console.log(this);
		console.log("&&&&" + encoded.toString(16));
		outputUInt32(out, encoded);
	}

	this.operator = operator;
	this.operand = operand;
};

var SymbolTable = function () {

	this.next = 0;
	this.map = {};

	/// Get without interning
	this.lookup = function (symbol) {
		return this.map[symbol];
	}

	/// Lookup and intern if not found.
	this.get = function (symbol) {
		var id = this.lookup(symbol);

		if (id == undefined) {
			
			console.log(this.map);
			id = this.next;
			this.next += 1;
			console.log("interning " + symbol + "with id " + id);
			this.map[symbol] = id;
		}

		return id;
	}

	/// callback(name, id)
	this.forEach = function (callback) {
		var me = this;
		for (symbol in this.map) {
			callback(symbol, me.map[symbol]);
		}
	}
}

var LabelTable = function () {

	this.table = [];

	this.create = function () {
		return table.push(undefined);
	}

	this.place = function (label, offset) {
		this.table[label] = offset;
	}

	this.createAndPlace = function (offset) {
		id = this.make();
		this.place(id, offset);
		return id;
	}

	this.instructionIndex = function (label) {
		return this.table[label];
	}

};

var LexicalContext = function (outer) {
	new SymbolTable();
	this.outer = outer;
};

/// A section of code. CodeBody has information about args and registers, but no information about indexes or it's name.
/// The name-to-index mapping is managed externally, by the module's FunctionTable. Eventually, the args and registers
/// might move to a lexical environment, and this will become a simple bytecode array.
function FunctionDefinition(outer) {
	this.args = new SymbolTable();
	this.regs = new SymbolTable();
	this.labels = new LabelTable();
	this.instructions = [];

	this.resolve = function (module) {
		for (instruction in this.instructions) {
			// TODO: Resolve any undefined function-id's or labels in the body.
			// 	switch(instruction.operator) {
			// 	case 
			// 	}

			// this.deltaForLabel = function (labelName) {
			// 	var gen = this;
			// 	return function (fromInstruction) {
			// 		// delay computing offset until forward labels are found
			// 		// output is done at the end, all labels will be defined
			// 		var fromIndex = fromInstruction.instructionIndex;
			// 		var toIndex = gen.labels[labelName];
			// 		return toIndex - fromIndex - 1;
			// 	}
			// };
		}
	}

	this.output = function (out) {
		// note that name and index are output by the module.
		outputUInt32(out, this.args.next);
		outputUInt32(out, this.regs.next);
		this.instructions.forEach(function (instruction) {
			instruction.output(out);
		})
	};

	this.pushInstruction = function (instruction) {
		this.instructions.push(instruction);
	};

	this.localIndex = function (name) {
		var index = this.regs.lookup(name);
		if (index) {
			return index + this.args.next;
		}
		return this.args.lookup(name);
	}

	this.placeLabel = function (label) {
		this.labels.place(label, instructions.length);
	}
};

/// The function table tracks function names and indexes. When a new name is encountered, the table automatically
/// creates a new stub function definition. Eventually, it's expected that the definition will be filled in, later in
/// the parsed script. Leaving any function undefined is an error.
function FunctionTable() {

	this.names = new SymbolTable();
	this.bodies = [];

	/// Look up the index of a 
	this.indexof = function (name) {
		return names.table.get(name);
	};

	/// Lookup a function's ID by name. If the function name hasn't been encountered before, reserve an ID for the name.
	this.get = function (name) {
		var id = this.names.get(name);
		if (!this.bodies[id]) {
			console.log("*** Reserving Function:" + name + " with index " + id);
			this.bodies[id] = null;
		}
		return id;
	};

	this.newFunctionDefinition = function (name, context) {
		var id = this.names.get(name);
		if (this.bodies[id]) {
			throw "Error: function defined twice";
		}
		console.error("*** Defining Function: " + name);
		var func = new FunctionDefinition(context);
		this.bodies[id] = func;
		return func;
	}

	/// callback(name, index, body)
	this.forEach = function (callback) {
		var me = this;
		this.names.forEach(function (name, index) {
			callback(name, index, me.bodies[index]);
		});
	};
};

function Module() {

	this.resolved = false;
	this.functions = new FunctionTable();
	this.strings = new SymbolTable();

	/// After the module has been entirely built up, resolve any undefined references.
	this.resolve = function () {
		console.log(this.functions);
		var me = this;
		this.functions.forEach(function(name, index, body) {

			if (!body) {
				throw "Undefined function reference: " + name;
			}
	
			console.log(index + ":" + name + " " + body);
			body.resolve(me);
		});
		this.resolved = true;
	}

	/// Output this module, in binary format. The Module must have been resolved.
	this.output = function (out) {
		if (!this.resolved) {
			throw "Module must be resolved before output."
		}

		this.outputHeader(out);
		this.outputFunctionSection(out);
		this.outputStringSection(out);
	}

	//
	// INTERNAL
	//

	this.outputHeader = function (out) {
		/// Raw magic bytes
		fs.writeSync(out, 'b9module');
	};

	/// internal
	this.outputFunctionSection = function (out) {
		var me = this;
		outputUInt32(out, 1); // the section code.
		outputUInt32(out, this.functions.bodies.length);
		this.functions.forEach(function (name, index, body) {
			me.outputFunction(out, name, index, body);
		});
	}

	this.outputFunction = function (out, name, index, body) {
		console.log(name);
		console.log(index);
		console.log(body);
		outputString(out, name);
		outputUInt32(out, index);
		body.output(out);
	}

	this.outputStringSection = function (out) {
		outputUInt32(out, 2); // the section code.
		outputUInt32(out, this.strings.next);
		this.strings.forEach(function (string, id) {
			outputString(out, string);
		});
	}
};

function FirstPassCodeGen() {

	this.module = undefined;
	this.func = undefined;

	this.compile = function (syntax) {
		console.log(syntax);
		this.module = new Module();
		var func = new FunctionDefinition(null); // top level

		this.handleBody(func, syntax.body);
		return this.module;
	};

	this.handleBody = function (func, body) {
		var me = this;
		body.forEach(function (element) {
			me.handle(func, element);
		});
	}

	this.handle = function (func, element) {
		if (element == null) {
			throw "Invalid element for code generation";
		}
		console.log("Element type: " + element.type);
		this.getHandler(element.type).call(this, func, element);
	};

	this.getHandler = function (elementType) {
		var name = "handle" + elementType;
		var handler = this[name];
		if (!handler) {
			throw "No handler for type " + elementType;
		}
		return handler;
	}

	/* STACK OPERATIONS */

	this.emitPushConstant = function (func, constant) {
		if (this.isNumber(constant)) {
			func.instructions.push(new Instruction("INT_PUSH_CONSTANT", constant));
		}
		else if (this.isString(constant)) {
			var id = this.module.strings.get(constant);
			func.instructions.push(new Instruction("STR_PUSH_CONSTANT", id));
		}
		// func.updateStackCount(1);
		else {
			throw "Unsupported constant/literal encountered: " + constant;
		}
	}

	this.emitPushFromVar = function (func, name) {
		var index = func.localIndex(name);
		func.instructions.push(new Instruction("PUSH_FROM_VAR", index));
		// this.currentFunction.updateStackCount(1);
	}

	this.emitPopIntoVar = function (func, name) {
		var index = func.localIndex(name);
		func.instructions.push(new Instruction("POP_INTO_VAR", index));
		// this.currentFunction.updateStackCount(-1);
	}

	this.handleFunctionDeclaration = function (func, declaration) {
		console.log("&&& Handle Function Declaration");
		console.log(declaration);
		var inner = this.module.functions.newFunctionDefinition(declaration.id.name, func);
		declaration.params.forEach(function (param) {
			inner.args.get(param);
		});
		inner.nargs = declaration.params.length;
		this.handle(inner, declaration.body);

		/// this discards the result of the last expression
		if (inner.instructions[inner.instructions.length - 1].operator != "FUNCTION_RETURN") {
			inner.instructions.push(new Instruction("INT_PUSH_CONSTANT", 0));
			inner.instructions.push(new Instruction("FUNCTION_RETURN", 0));
		}

		inner.instructions.push(new Instruction("END_SECTION", 0));
	};

	this.handleAssignmentExpression = function (func, expression) {
		var assignmentOpToInstruction = {
			"+=": "ADD",
			"-=": "SUB",
			"/=": "DIV",
			"*=": "MUL",
		};

		if (expression.left.type == "Identifier") {
			var operator = this.assignmentOpToInstruction[expression.operator];
			if (operator) {
				this.handle(expression.left); // extra left
				this.handle(expression.right);
				func.instructions.push(new Instruction(operator, 0));
				// this.currentFunction.updateStackCount(-1);
			} else {
				expression.right.needResult = true;
				this.handle(expression.right);
			}

			if (expression.needResult === true) {
				func.instructions.push(new Instruction("DUPLICATE"));
			}

			this.emitPopVar(expression.left.name);

			if (expression.isParameter == true) {
				this.emitPushVar(expression.left.name);
			}
			return;
		}
		this.handle(expression.right);
	};

	this.handleVariableDeclaration = function (func, declaration) {
		/// composed of potentially multiple variables.
		this.handleAll(func, declaration.declarations);
	}

	this.handleVariableDeclarator = function (func, declarator) {
		var id = func.regs.get(declarator.id.name);
		if (decl.init) {
			this.handle(decl.init);
			this.popvar(decl.id.name);
		}
	};

	this.handleExpressionStatement = function (func, statement) {
		this.handle(func, statement.expression);
		func.instructions.push(new Instruction("DROP"));
	};

	// Iterate expressions array
	this.handleSequenceExpression = function (func, sequence) {
		var expressions = sequence.expressions;
		var droplast = !decl.isParameter;

		for (expression in sequence.expressions.slice(0, -1)) {
			this.handle(func, expression);
			func.instructions.push(new Instruction("DROP"));
		}

		var last = sequence.expressions.slice(-1)[0];
		this.handle(last);

		if (!sequence.isParameter) {
			func.instructions.push(new Instruction("DROP"));
		}
	};

	this.handleUnaryExpression = function (func, decl) {
		if (decl.operator == "-" && decl.argument.type == "Literal") {
			func.instructions.push(new Instruction("INT_PUSH_CONSTANT", - decl.argument.value));
			// this.currentFunction.updateStackCount(1);
			return;
		}
		if (decl.operator == "+") {
			this.handle(func, decl.argument);
			return;
		}
		if (decle.operator == "-") {
			this.pushconstant(0);
			func.instructions.push(new Instruction("INT_PUSH_CONSTANT", 0));
			this.handle(func, decl.argument);
			func.instructions.push(new Instruction("INT_SUB"));
			return;
		}
		if (decl.operator == "!") {
			this.handle(func, decl.argument);
			func.instructions.push(new Instruction("INT_NOT"));
			return;
		}
		throw "halt", "Error - No Handler for Type: " + decl;
	};

	this.compareOpToInstruction = function (code) {
		if (code == "==") instruction = "INT_JMP_NEQ";
		else if (code == "!=") instruction = "INT_JMP_EQ";
		else if (code == "<=") instruction = "INT_JMP_GT";
		else if (code == "<") instruction = "INT_JMP_GE";
		else if (code == ">") instruction = "INT_JMP_LE";
		else if (code == ">=") instruction = "INT_JMP_LT";
		else throw "Unhandled code";
		return instruction;
	};

	this.handleBinaryExpression = function (func, decl) {
		this.handle(decl.left);
		this.handle(decl.right);
		if (decl.operator == "-") {
			func.instructions.push(new Instruction("INT_SUB"));
		}
		else if (decl.operator == "+") {
			func.instructions.push(new Instruction("INT_ADD"));
		}
		else if (decl.operator == "*") {
			func.instructions.push(new Instruction("INT_MUL"));
		}
		else if (decl.operator == "/") {
			func.instructions.push(new Instruction("INT_DIV"));
		}
		else {
			var code = this.compareOpToInstruction(decl.operator);
			if (!code) {
				throw "This operator is not being handled" + decl.operator;
			}
			return code;
		}
		return null;
	};

	this.handleBlockStatement = function (func, decl) {
		this.handleBody(func, decl.body);
	}

	this.handleUpdateExpression = function (func, decl) {
		if (decl.argument.type != "Identifier") {
			throw "Invalid Update Statement support variables only";
		}

		this.emitPushFromVar(func, decl.argument.name);
		func.instruction.push(new Instruction("INT_PUSH_CONSTANT", 1));

		if (decl.operator == "++") {
			func.instructions.push(new Instruction("INT_ADD"));
		}
		else if (decl.operator == "--") {
			func.instructions.push(new Instruction("INT_SUB"));
		}

		this.emitPopIntoVar(decl.argument.name);
	};

	this.handleForStatement = function (func, decl) {
		var loopTest = "@loopTest" + this.label;
		var loopEnd = "@loopEnd" + this.label;
		var loopContinue = "@loopContinue" + this.label;
		this.label++;

		this.savehack(function () {
			console.log("// -- init ---");

			if (decl.init) this.handle(decl.init);

			console.log("// -- test ---");

			this.placeLabel(loopTest);
			if (decl.test == undefined) {
				code = "nojump";
			} else {
				var code = this.handle(decl.test);
			}
			var instruction = this.genJmpForCompare(code, "FOR");
			this.outputInstruction(instruction, this.deltaForLabel(loopEnd), "genJmpForCompare FOR " + code);
		});

		this.flowControlBreakContinue(loopEnd, loopContinue, "FOR",
			function () {
				this.savehack(function () {
					this.handle(decl.body);
					this.placeLabel(loopContinue);
					decl.needResult = false;
					if (decl.update) decl.update.needResult = false;
					var expected = this.currentFunction.pushcount;
					this.handle(decl.update);
					this.currentFunction.dropTOS(expected);
					this.outputInstruction("JMP", this.deltaForLabel(loopTest), "FOR LOOP");
					this.placeLabel(loopEnd);
				});
			});
	};

	this.handleCallExpression = function (func, expression) {
		// Set up arguments for call

		if (expression.callee.type != "Identifier") {
			throw "Only handles named functions";
		}

		if (!expression.callee.name) {
			throw "Trying to compile call to function with no name";
		}

		/// TODO: Only supports calling functions by name
		if (expression.callee.name == "b9_primitive") {
			this.emitPrimitiveCall(func, expression)
		}
		else {
			this.emitFunctionCall(func, expression);
		}
	}


	this.handleReturnStatement = function (func, decl) {
		if (decl.argument != null) {
			this.handle(func, decl.argument);
		}
		this.genReturn(false);
	};

	this.emitFunctionCall = function (func, expression) {
		// TODO: what is this doing?
		expression.arguments.forEach(function (element) {
			element.isParameter = true;
		});
		this.handleBody(func, expression.arguments);
		var target = this.module.functions.get(expression.callee.name);
		func.instructions.push(new Instruction("FUNCTION_CALL", target));
	}

	this.emitPrimitiveCall = function (func, expression) {
		/// The first argument is a "phantom" argument that tells us the primitive code.
		/// It's not compiled as an expression.
		var code = primitiveCode(expression.arguments[0].value);

		var args = expression.arguments.slice(1);
		args.forEach(function (element) {
			element.isParameter = true;
		});
		this.handleBody(func, args);
		func.instructions.push(new Instruction("PRIMITIVE_CALL", code));
		return true;
	};

	this.genJmpForCompare = function (code) {
		if (code == "==") instruction = "INT_JMP_NEQ";
		else if (code == "!=") instruction = "INT_JMP_EQ";
		else if (code == "<=") instruction = "INT_JMP_GT";
		else if (code == "<") instruction = "INT_JMP_GE";
		else if (code == ">") instruction = "INT_JMP_LE";
		else if (code == ">=") instruction = "INT_JMP_LT";
		else throw "Unhandled code";
		return instruction;
	};

	this.genReturn = function (func, forced) {
		if (func.instructions[func.instructions.length - 1].operand == "FUNCTION_RETURN") {
			console.error("Warning: generating duplicate returns");
			return;
		}

		if (this.currentFunction.pushcount == 0) {
			this.outputInstruction("INT_PUSH_CONSTANT", 0, " Generate Free Return");
		}

		this.outputInstruction("FUNCTION_RETURN", 0, " forced = " + forced);
	}

	this.genEndOfByteCodes = function () {
		this.outputInstruction("END_SECTION", 0, "");
	};

	this.declareFunction = function (id, decl) {
		var newFunction = { index: this.nextFunctionIndex++, name: id, nargs: -1, nregs: -1 };
		this.functions[id] = newFunction;
	};

	/* HANDLE JUMPS AND LABELS */


	this.flowControlBreakContinue = function (breakLabel, continueLabel, location, body) {
		var saveBreak = this.currentBreak;
		this.currentBreak = function () {
			this.outputInstruction("JMP", this.deltaForLabel(breakLabel), "break in " + location);
		}
		var saveContinue = this.currentContinue;
		this.currentContinue = function () {
			this.outputInstruction("JMP", this.deltaForLabel(continueLabel), "continue in " + location);
		}

		body.call(this);
		this.currentBreak = saveBreak;
		this.currentContinue = saveContinue;
	};

	this.isNumber = function isNumber(num) {
		return typeof num == "number";
	};

	this.isString = function isString(num) {
		return typeof num == 'string';
	};

	this.getStringIndex = function (id) {
		return this.strings.lookup(string);
		if (this.strings[id] != undefined) {
			return this.strings[id];
		} else {
			this.strings[id] = this.nextStringIndex++;
			return this.strings[id];
		}
	};

	this.handleLiteral = function (func, literal) {
		this.emitPushConstant(func, literal.value);
	}

	this.handleIfStatement = function (func, decl) {

		var falseLabel = func.labels.create();
		var endLabel = func.labels.create();

		var comparator = this.handle(func, decl.test);

		var jumpOperator = null;

		if (decl.test.type == "BinaryExpression") {
			// Binary expressions compile to specialized JMP operations
			jumpOperator = this.compareOpToInstruction(comparator, "IF");
		} else {
			// Unary expressions always compile to a comparison with false.
			func.instructions.push(new Instruction("INT_PUSH_CONSTANT", 0));
			jumpOperator = "INT_JMP_EQ";
		}

		if (decl.alternate != null) {
			this.outputInstruction(instruction, this.deltaForLabel(labelF),
				"genJmpForCompare has false block, IF " + code);
		} else {
			this.outputInstruction(instruction, this.deltaForLabel(labelEnd),
				"genJmpForCompare has no false block, IF " + code);
		}

		// this.savehack(function () { // true part  is fall through from genJmpForCompare
		if (decl.alternate != null) {
			// you only have a false if there is code
			// so you only jump if there is code to jump around
			if (this.prevInstruction.bc != "FUNCTION_RETURN") {
				this.outputInstruction("JMP", this.deltaForLabel(labelEnd), "SKIP AROUND THE FALSE CODE BLOCK");
			}
			this.placeLabel(labelF);
			this.handle(decl.alternate);
		}
		this.placeLabel(labelEnd);
	};

	this.handleEmptyStatement = function (decl) { };

	this.handleWhileStatement = function (func, statement) {
		var loopTest = func.labels.create();
		var loopEnd = func.labels.create();
		var loopContinue = func.labels.create();

		var test = func.labels.create();
		func.placeLabel(test);
		var comparator = this.handle(func, statement.test);
		var jumpOperator = this.compareOpToInstruction(code);

		func.body.push_back(new Instruction(jumpOperator, end));

		func.labels.place(loopEnd);

		this.savehack(function () {
			this.placeLabel(loopTest);
			var code = this.handle(decl.test);

			this.outputInstruction(instruction,
				this.deltaForLabel(loopEnd), "genJmpForCompare WHILE " + code);
		});

		this.flowControlBreakContinue(loopEnd, loopContinue, "WHILE", function () {
			this.savehack(function () {
				this.handle(decl.body);
				decl.needResult = false;
				this.placeLabel(loopContinue);
				this.outputInstruction("JMP", this.deltaForLabel(loopTest), "WHILE ");
				this.placeLabel(loopEnd);
			});
		});
	};
};

/// Compile without resolving symbols.
function compile0(syntax) {
	var compiler = new FirstPassCodeGen();
	return compiler.compile(syntax);
}

/// Compile and output a complete module.
/// Compilation happens in 3 phases:
///  1. Parse -- translate a JS program to a syntax tree.
///  2. Compile0 -- first pass compilation of the program to a module.
///  3. resolve -- final stage of linking up unresolved reference in the input program.
function compile(code, output) {
	var syntax = esprima.parse(code);
	var module = compile0(syntax);
	module.resolve();
	module.output(output);
	return true;
};

function main() {
	if (process.argv.length != 4) {
		console.error("Usage: node.js compile.js <infile> <outfile>");
		process.exit(1);
	}

	inputPath = process.argv[2];
	outputPath = process.argv[3];

	var code = fs.readFileSync(__dirname + "/b9stdlib.src", 'utf-8');
	code += fs.readFileSync(inputPath, 'utf-8');

	// console.log(code);
	output = fs.openSync(outputPath, "w");
	console.log(outputPath + " AAAAA " + output);
	compile(code, output);
};

main();