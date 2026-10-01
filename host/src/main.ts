#!/usr/bin/env node
import { CliApplication } from '#cli/CliApplication.ts';
import { DevicesCommand } from '#cli/commands/DevicesCommand.ts';
import { DoctorCommand } from '#cli/commands/DoctorCommand.ts';
import { InstallCommand, StatusCommand, UninstallCommand } from '#cli/commands/InstallCommands.ts';
import { ProbeCommand } from '#cli/commands/ProbeCommand.ts';
import { StartCommand } from '#cli/commands/StartCommand.ts';

const cli = new CliApplication([
  new StartCommand(),
  new DevicesCommand(),
  new ProbeCommand(),
  new DoctorCommand(),
  new StatusCommand(),
  new InstallCommand(),
  new UninstallCommand(),
]);

process.exitCode = await cli.run(process.argv.slice(2));
